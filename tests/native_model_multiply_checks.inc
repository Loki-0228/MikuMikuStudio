// Include after native_bone_registration_checks.inc, native_morph_key_checks.inc
// and native_camera_multiply_checks.inc. RealMenu drives actual 251/252 dialogs.
#include "../src/plugins/native_model_multiply.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include <limits>
namespace mikumikustudio {
bool ApplyBoneFrameScaleAdd(MMDApp*, HWND);
void ApplyMorphScaleAdd(MMDApp*, HWND);
void CmdViewMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}
namespace model_multiply_checks {
using namespace plugins;
using Kind = ModelMultiplyKind;
using Mode = SharedHistory::RecordingMode;
struct Fixture : MorphKeyFixture {
    HWND dialog = nullptr;
    Fixture() {
        app->HInstance() = GetModuleHandleW(nullptr); app->EnglishUI() = 1;
        app->CurrentFrame() = 17; app->PlaybackPhysicsMode() = 0;
        for (auto* source : {&first, &second}) {
            SeedRegistrationChain(*source); MorphKeySeed(*source);
            source->m->physicsMode = 2;
            for (unsigned bone = 0; bone < 2; ++bone) {
                source->bones[bone].type = mdl::BoneType::RotateMove; source->dirty[bone] = 1;
                source->bones[bone].trans[2] = 91.f + bone;
            }
            for (unsigned key : {0u, 1u, 2u}) source->m->boneKeys[key].allocated = 3;
            source->m->boneKeys[3].allocated = 0;
            auto shear = bone_scale::Key(source->raw(), 2); shear.v[1] = .75f; shear.v[5] = .25f; shear.v[6] = -.1f;
            bone_scale::SetKey(source->raw(), 2, shear); bone_scale::SetKeyCurve(source->raw(), 2, {17, 66, 98, 113});
            Check(bone_scale::Key(source->raw(), 2) == shear && bone_scale::KeyCurve(source->raw(), 2) == bone_scale::Curve{17, 66, 98, 113},
                "seed nonuniform sheared key scale and an independent easing curve");
            for (unsigned key : {0u, 6u, 7u, 8u}) source->m->morphKeys[key].allocated = 5;
            source->m->displayKeys[0].allocated = 7;
        }
        dialog = CreateWindowExW(0, L"STATIC", L"model multiply inputs", WS_OVERLAPPED, 0, 0, 400, 300,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(dialog != nullptr, "create hidden model multiply input parent");
        for (int id = 686; id <= 703; ++id)
            Check(CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 0, 80, 20, dialog,
                reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr) != nullptr,
                "create real original and scale-channel multiply edit IDs");
    }
    ~Fixture() { DestroyWindow(dialog); }
    void Set(const BoneMultiplyOptions& options, HWND target = nullptr) {
        if (!target) target = dialog;
        for (unsigned lane = 0; lane < 9; ++lane) for (unsigned pair = 0; pair < 2; ++pair) {
            char text[64]{}; std::snprintf(text, sizeof text, "%.9g", double(pair ? options.offset[lane] : options.scale[lane]));
            Check(SetDlgItemTextA(target, 686 + int(2 * lane + pair), text) != FALSE, "populate round-trip bone multiply numbers");
        }
    }
    void Set(const MorphMultiplyOptions& options, HWND target = nullptr) {
        if (!target) target = dialog;
        for (unsigned pair = 0; pair < 2; ++pair) {
            char text[64]{}; std::snprintf(text, sizeof text, "%.9g", double(pair ? options.offset : options.scale));
            Check(SetDlgItemTextA(target, 686 + int(pair), text) != FALSE, "populate original morph multiply numbers");
        }
    }
    std::shared_ptr<HistoryChange> Change(Kind kind, bool identity = false) {
        NativeModelMultiplyPlan plan; std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*app, kind) == MMS_OK, "capture model multiply source before modal editing");
        if (kind == Kind::Bone) Set(identity ? BoneMultiplyOptions{} : BoneOptions());
        else Set(identity ? MorphMultiplyOptions{} : MorphOptions());
        Check(plan.ReadDialog(dialog) == MMS_OK && plan.Take(change) == MMS_OK,
            "real controls build an immutable model multiply participant"); return change;
    }
    static BoneMultiplyOptions BoneOptions() {
        BoneMultiplyOptions options; options.scale = {1.25f, -.5f, 2.f, .75f, -1.f, 1.5f, 1.5f, .8f, 2.f};
        options.offset = {3.25f, -2.5f, .125f, 13.25f, -27.5f, 33.75f, .25f, .1f, .2f}; return options;
    }
    static MorphMultiplyOptions MorphOptions() { return {-1.75f, .375f}; }
    void Oracle(Kind kind) {
        const auto previous = g_Block; g_Block = app.get();
        auto renderer = std::make_unique<D3DRenderer>(); const auto oldRenderer = app->Renderer(); app->Renderer() = renderer.get();
        if (kind == Kind::Bone) { Set(BoneOptions()); Check(ApplyBoneFrameScaleAdd(app.get(), dialog), "actual unbound bone multiply accepts oracle inputs"); }
        else { Set(MorphOptions()); ApplyMorphScaleAdd(app.get(), dialog); }
        app->Renderer() = oldRenderer; g_Block = previous;
    }
    std::uint64_t Digest() {
        std::uint64_t hash = first.Digest() ^ (second.Digest() << 1);
        const auto add = [&](const void* bytes, std::size_t count) { auto* p = static_cast<const unsigned char*>(bytes);
            while (count--) { hash ^= *p++; hash *= 1099511628211ull; } };
        add(firstMorphs.data(), sizeof(firstMorphs)); add(secondMorphs.data(), sizeof(secondMorphs));
        for (auto* source : {&first, &second}) {
            for (unsigned key = 0; key < 32; ++key) { const auto value = bone_scale::Key(source->raw(), int(key));
                const auto curve = bone_scale::KeyCurve(source->raw(), int(key)); add(&value, sizeof value); add(&curve, sizeof curve); }
            for (unsigned bone = 0; bone < 2; ++bone) { const auto value = bone_scale::Pose(source->raw(), int(bone)); add(&value, sizeof value); }
        }
        return hash;
    }
};
void Commit(Fixture& fixture, const std::shared_ptr<HistoryChange>& change) {
    Check(bool(change), "multiply owns a substantive record"); std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(fixture.history.PrepareNative("model multiply", change, transaction, SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK,
        "prepare one explicit model multiply node");
    Check(CommitRegistrationNoAllocation(fixture.history, std::move(transaction)) == MMS_OK,
        "keys authoring and local clean-pose evaluation publish without allocation");
}
void ParityAndCrossModel() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (auto kind : {Kind::Bone, Kind::Morph}) {
        Fixture fixture, oracle; NativePoseTestBinding binding(*fixture.app, fixture.history);
        Check(fixture.history.SetMode(mode) == MMS_OK, "select explicit or all-operation model multiply history");
        const auto beforeBone = RegistrationKeys(fixture.first); const auto beforeMorph = MorphKeysCopy(fixture.first);
        const auto pose = fixture.first.bones; const auto weights = fixture.firstMorphs;
        const auto ring = PoseRingDigest(fixture.first), before = fixture.Digest();
        const auto maximum = fixture.first.m->maxFrame; const auto cursor = fixture.first.m->searchCursor;
        const auto last = fixture.app->LastRegisteredFrame();
        auto change = fixture.Change(kind);
        Check(fixture.Digest() == before, "modal input preparation writes no native pose key mark scale or legacy history");
        Commit(fixture, change); oracle.Oracle(kind);
        resource_budget::Collector bookkeeping;
        const auto owner = RetainSceneObject(*fixture.app, MMS_OBJECT_MODEL, 0);
        Check(owner->MeasureBookkeeping(bookkeeping) == MMS_OK && bookkeeping.Result().Complete() &&
            fixture.history.RetainedBytes() >= resource_budget::Collector::Sum(change->RetainedBytes(), bookkeeping.Result().totalBytes) &&
            !change->RequiresPhysicsReset(), "model multiply charges retained owner bookkeeping without a global physics reset");
        Check(SameRegistrationKeys(fixture.first, kind == Kind::Bone ? RegistrationKeys(oracle.first) : beforeBone) &&
            MorphKeysMatch(fixture.first, kind == Kind::Morph ? MorphKeysCopy(oracle.first) : beforeMorph),
            "selected roots and sparse keys match actual legacy float and quaternion arithmetic while other families survive");
        for (unsigned key = 0; key < 32; ++key)
            Check(bone_scale::Key(fixture.first.raw(), int(key)) == bone_scale::Key(kind == Kind::Bone ? oracle.first.raw() : fixture.second.raw(), int(key)) &&
                bone_scale::KeyCurve(fixture.first.raw(), int(key)) == bone_scale::KeyCurve(fixture.second.raw(), int(key)),
                "bone scale row directions and existing interpolation retain the legacy matrix results");
        Check(!std::memcmp(pose.data(), fixture.first.bones.data(), sizeof pose) &&
            !std::memcmp(weights.data(), fixture.firstMorphs.data(), sizeof weights) && PoseRingDigest(fixture.first) == ring &&
            fixture.first.m->maxFrame == maximum && fixture.first.m->searchCursor == cursor && fixture.app->LastRegisteredFrame() == last &&
            fixture.history.UndoCount() == 1 && fixture.app->CurrentFrame() == 17,
            "one node in either mode preserves unregistered bone and morph poses metadata playhead and legacy rings");
        auto afterBone = RegistrationKeys(fixture.first); auto afterMorph = MorphKeysCopy(fixture.first);
        auto inverseBone = beforeBone; auto inverseMorph = beforeMorph;
        fixture.app->SelectedModelSlot() = 1; fixture.app->CameraMode() = 1; fixture.app->CurrentFrame() = 37;
        for (unsigned key : {0u, 1u, 2u, 3u}) inverseBone[key].allocated = afterBone[key].allocated = fixture.first.m->boneKeys[key].allocated = 11;
        for (unsigned key : {0u, 6u, 7u, 8u}) inverseMorph[key].allocated = afterMorph[key].allocated = fixture.first.m->morphKeys[key].allocated = 13;
        Check(fixture.history.History(false) == MMS_OK && SameRegistrationKeys(fixture.first, inverseBone) && MorphKeysMatch(fixture.first, inverseMorph),
            "undo follows retained model identity from another selected model and camera mode while preserving later marks");
        Check(fixture.history.History(true) == MMS_OK && SameRegistrationKeys(fixture.first, afterBone) && MorphKeysMatch(fixture.first, afterMorph) &&
            fixture.app->CurrentFrame() == 37 && !std::memcmp(weights.data(), fixture.firstMorphs.data(), sizeof weights),
            "redo restores owned values without loading source files or replacing current unregistered weights");
    }
}
void CleanEvaluation() {
    for (auto kind : {Kind::Bone, Kind::Morph}) for (int frame : {0, 5, 17, 35}) {
        Fixture fixture, oracle; NativePoseTestBinding binding(*fixture.app, fixture.history);
        fixture.app->CurrentFrame() = oracle.app->CurrentFrame() = frame;
        fixture.first.dirty.fill(0); oracle.first.dirty.fill(0);
        SeekModelFrame(fixture.first.raw(), frame, 0); SeekModelFrame(oracle.first.raw(), frame, 0);
        const auto positions = fixture.first.bones; const auto weights = fixture.firstMorphs;
        std::array<bone_scale::Matrix, 2> scale{bone_scale::Pose(fixture.first.raw(), 0), bone_scale::Pose(fixture.first.raw(), 1)};
        auto change = fixture.Change(kind); Commit(fixture, change); oracle.Oracle(kind);
        if (kind == Kind::Bone) {
            for (unsigned bone = 0; bone < 2; ++bone)
                Check(!std::memcmp(fixture.first.bones[bone].trans, oracle.first.bones[bone].trans, sizeof positions[bone].trans) &&
                    !std::memcmp(fixture.first.bones[bone].rotQuat, oracle.first.bones[bone].rotQuat, sizeof positions[bone].rotQuat) &&
                    bone_scale::Pose(fixture.first.raw(), int(bone)) == bone_scale::Pose(oracle.first.raw(), int(bone)),
                    "private clean-bone evaluation matches actual native seek at root interpolated and tail frames");
        } else {
            for (unsigned morph = 0; morph < 3; ++morph)
                Check(MorphSameFloat(fixture.firstMorphs[morph].value, oracle.firstMorphs[morph].value),
                    "private clean-morph evaluation matches actual native float sampling at the saved playhead");
        }
        Check(fixture.app->CurrentFrame() == frame && fixture.history.History(false) == MMS_OK,
            "clean multiply and undo leave the live playhead fixed");
        if (kind == Kind::Bone) for (unsigned bone = 0; bone < 2; ++bone)
            Check(!std::memcmp(fixture.first.bones[bone].trans, positions[bone].trans, sizeof positions[bone].trans) &&
                !std::memcmp(fixture.first.bones[bone].rotQuat, positions[bone].rotQuat, sizeof positions[bone].rotQuat) &&
                bone_scale::Pose(fixture.first.raw(), int(bone)) == scale[bone], "clean-bone undo restores the original current sample");
        else for (unsigned morph = 0; morph < 3; ++morph)
            Check(MorphSameFloat(fixture.firstMorphs[morph].value, weights[morph].value), "clean-morph undo restores its original current sample");
    }
}
void Authors() {
    Fixture fixture; NativePoseTestBinding binding(*fixture.app, fixture.history); std::string error;
    Check(bone_curve::Write(fixture.first.raw(), 0, RegistrationFreeTracks(), error), "seed free position rotation and scale lanes");
    for (std::size_t key = 0; key < mdl::BoneKeyCapacity(fixture.first.m->boneKeys); ++key)
        fixture.first.m->boneKeys[key].allocated = fixture.first.m->boneKeys[key].frame == 20 ? 3 : 0;
    const auto boneBefore = bone_curve::AuthoringState::Capture(fixture.first.raw());
    const auto scaleBefore = bone_scale::AuthoringState::Capture(fixture.first.raw());
    Commit(fixture, fixture.Change(Kind::Bone));
    const auto boneAfter = bone_curve::AuthoringState::Capture(fixture.first.raw());
    const auto scaleAfter = bone_scale::AuthoringState::Capture(fixture.first.raw());
    bone_curve::Tracks tracks; Check(bone_curve::ReadDetached(fixture.first.raw(), 0, tracks) &&
        tracks[0].keys.front().outWeight == .2 && tracks[0].keys.back().inWeight == .4,
        "native value and scale multiplication reconciles free authoring without replacing its endpoint handles");
    Check(fixture.history.History(false) == MMS_OK && bone_curve::AuthoringState::Capture(fixture.first.raw())->Equals(*boneBefore) &&
        bone_scale::AuthoringState::Capture(fixture.first.raw()).Equals(scaleBefore), "undo restores exact free and scale snapshots");
    Check(fixture.history.History(true) == MMS_OK && bone_curve::AuthoringState::Capture(fixture.first.raw())->Equals(*boneAfter) &&
        bone_scale::AuthoringState::Capture(fixture.first.raw()).Equals(scaleAfter), "redo restores exact transformed free and scale snapshots");
    Check(fixture.history.Reset() == MMS_OK, "start independent morph authoring history"); fixture.history.Collect();
    const auto original = MorphAuthoredTrack(fixture.firstMorphs[1], 1);
    Check(morph_curve::Write(fixture.first.raw(), {original}, error), "seed pristine free morph tangents");
    morph_curve::AuthoredLayers layers;
    Check(morph_curve::CaptureLayers(fixture.first.raw(), layers, error), "capture complete pristine effective and mirror source");
    const auto before = MorphKeysCopy(fixture.first); const auto weights = fixture.firstMorphs;
    Commit(fixture, fixture.Change(Kind::Morph));
    Check(morph_curve::LayersUnchanged(fixture.first.raw(), layers) &&
        !std::memcmp(weights.data(), fixture.firstMorphs.data(), sizeof weights),
        "morph multiply preserves pristine layers and unregistered current weights while native target drives derivation");
    Check(fixture.history.History(false) == MMS_OK && MorphKeysMatch(fixture.first, before), "undo restores native mirrors for pristine free morphs");
    std::vector<animation_curve::Track> restored; std::wstring wideError;
    Check(morph_curve::ReadManyDetached(*fixture.app, {1}, restored, wideError) && restored.size() == 1 &&
        restored[0].keys.front().outSlope == original.track.keys.front().outSlope &&
        restored[0].keys.back().inWeight == original.track.keys.back().inWeight,
        "native undo revives the exact original morph author handles without a file read");
}
void CancelNoopInvalidAndBudget() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (auto kind : {Kind::Bone, Kind::Morph}) {
        Fixture fixture; NativePoseTestBinding binding(*fixture.app, fixture.history);
        Check(fixture.history.SetMode(mode) == MMS_OK, "choose no-op recording mode");
        if (kind == Kind::Bone) fixture.first.m->boneKeys[0].position[0] = -0.f;
        else fixture.first.m->morphKeys[0].value = -0.f;
        Commit(fixture, fixture.Change(kind)); Check(fixture.history.History(false) == MMS_OK, "seed model multiply redo branch");
        const auto before = fixture.Digest(); const auto redo = fixture.history.RedoCount();
        { NativeModelMultiplyPlan cancel; Check(cancel.Begin(*fixture.app, kind) == MMS_OK, "begin cancellable modal source"); }
        Check(!fixture.Change(kind, true) && fixture.Digest() == before && fixture.history.RedoCount() == redo,
            "cancel and identity OK preserve the entire source and existing redo in both recording modes");
        Check(std::signbit(kind == Kind::Bone ? fixture.first.m->boneKeys[0].position[0] : fixture.first.m->morphKeys[0].value),
            "identity multiply preserves a selected signed-zero key exactly");
        NativeModelMultiplyPlan invalid; Check(invalid.Begin(*fixture.app, kind) == MMS_OK, "capture invalid-input source");
        if (kind == Kind::Bone) {
            auto options = Fixture::BoneOptions(); options.offset[8] = -100;
            Check(invalid.Read(options) == MMS_E_ARGUMENT, "negative target scale rejects every selected key atomically");
            options = Fixture::BoneOptions(); options.scale[0] = std::numeric_limits<float>::infinity();
            Check(invalid.Read(options) == MMS_E_ARGUMENT, "nonfinite bone multiplier is refused");
        } else {
            Check(invalid.Read(MorphMultiplyOptions{std::numeric_limits<float>::max(), 0}) == MMS_E_ARGUMENT,
                "overflow in a later selected morph key rejects the complete operation");
            Check(invalid.Read(MorphMultiplyOptions{1, std::numeric_limits<float>::quiet_NaN()}) == MMS_E_ARGUMENT,
                "nonfinite morph offset is refused");
        }
        std::shared_ptr<HistoryChange> absent;
        Check(invalid.Take(absent) == MMS_E_NOT_READY && !absent && fixture.Digest() == before && fixture.history.RedoCount() == redo,
            "failed input leaves no prepared partial edit and preserves redo");
        auto change = fixture.Change(kind);
        Check(fixture.history.Configure(mode, 128, 1) == MMS_OK, "configure a real insufficient native-history budget");
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(fixture.history.PrepareNative("over-budget multiply", change, transaction) == MMS_E_MEMORY && !transaction && fixture.Digest() == before,
            "history budget rejection leaves every native and authoring value untouched");
    }
}
void StaleAndAllocation() {
    for (auto kind : {Kind::Bone, Kind::Morph}) {
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            Fixture fixture; NativePoseTestBinding binding(*fixture.app, fixture.history); NativeModelMultiplyPlan plan;
            Check(plan.Begin(*fixture.app, kind) == MMS_OK, "capture pre-modal ownership and selected keys");
            if (mutation == 0) fixture.app->SelectedModelSlot() = 1;
            else if (mutation == 1) fixture.app->CurrentFrame() += 1;
            else if (mutation == 2) {
                if (kind == Kind::Bone) fixture.first.m->boneKeys[2].allocated ^= 1;
                else fixture.first.m->morphKeys[7].allocated ^= 1;
            } else { NativePoseTestBinding temporary(*fixture.app, fixture.history); }
            const auto before = fixture.Digest();
            Check((kind == Kind::Bone ? plan.Read(Fixture::BoneOptions()) : plan.Read(Fixture::MorphOptions())) == MMS_E_STALE &&
                fixture.Digest() == before, "model selection playhead key marks or a recycled binding invalidates the old modal source");
        }
        Fixture fixture; NativePoseTestBinding binding(*fixture.app, fixture.history);
        const auto before = fixture.Digest(); bool completed = false; unsigned failures = 0;
        for (long limit = 0; limit < 1024; ++limit) {
            NativeModelMultiplyPlan plan; failAfter = limit;
            auto result = plan.Begin(*fixture.app, kind);
            if (result == MMS_OK) result = kind == Kind::Bone ? plan.Read(Fixture::BoneOptions()) : plan.Read(Fixture::MorphOptions());
            std::shared_ptr<HistoryChange> change;
            if (result == MMS_OK) result = plan.Take(change);
            failAfter = -1;
            Check(fixture.Digest() == before, "allocation fault in source capture or private planning cannot write partial native data");
            if (result == MMS_OK) { Check(bool(change), "completed fault-sweep plan owns its substantive data"); completed = true; break; }
            Check(result == MMS_E_MEMORY, "private model multiply allocation reports memory failure"); ++failures;
        }
        Check(completed && failures > 3, "model multiply source and authoring allocation sweep reaches success");
        auto change = fixture.Change(kind); std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        Check(fixture.history.PrepareNative("stale multiply publication", change, transaction) == MMS_OK, "prepare fully validated multiply publication");
        if (kind == Kind::Bone) fixture.first.m->boneKeys[2].position[0] += 7;
        else fixture.first.m->morphKeys[7].value += .2f;
        const auto edited = fixture.Digest();
        Check(fixture.history.CommitNative(std::move(transaction)) == MMS_E_STALE && fixture.Digest() == edited && fixture.history.UndoCount() == 0,
            "changes after preparation refuse publication without overriding the newer source");
    }
}
void PluginComposition() {
    Fixture fixture; NativePoseTestBinding binding(*fixture.app, fixture.history); camera_multiply_checks::PluginState plugin;
    StorageClient storage; storage.id = "test.model-multiply"; fixture.storage.Api(storage);
    auto serializer = plugin.Serializer(); MmsHandle provider = 0;
    Check(fixture.storage.Register(storage, Text(storage.id), &serializer, sizeof serializer, &provider) == MMS_OK, "register real model-multiply plugin serializer");
    EditStore edits(fixture.history, fixture.storage); EditClient client; client.id = storage.id; auto api = edits.Api(client);
    const auto before = RegistrationKeys(fixture.first); auto bone = fixture.Change(Kind::Bone);
    std::uint64_t revision = 0; MmsHandle transaction = 0;
    Check(fixture.history.Revision(revision) == MMS_OK && api.begin(api.context, Text("mixed model multiply"), revision, &transaction) == MMS_OK,
        "begin a real shared plugin transaction");
    NativeModelMultiplyPlan busy; Check(busy.Begin(*fixture.app, Kind::Morph) == MMS_E_BUSY, "an active plugin draft excludes another multiply modal");
    auto participant = plugin.Participant(Text(client.id));
    Check(api.join(api.context, transaction, &participant) == MMS_OK && edits.JoinNative(client, transaction, bone) == MMS_OK &&
        api.prepare(api.context, transaction) == MMS_OK && api.commit(api.context, transaction) == MMS_OK && plugin.value == 42,
        "plugin state and model multiply publish as one prepared common-history transaction");
    const auto after = RegistrationKeys(fixture.first);
    fixture.app->SelectedModelSlot() = 1; const auto otherBefore = MorphKeysCopy(fixture.second);
    Commit(fixture, fixture.Change(Kind::Morph)); const auto otherAfter = MorphKeysCopy(fixture.second);
    Check(api.undo(api.context) == MMS_OK && MorphKeysMatch(fixture.second, otherBefore) && plugin.value == 42 &&
        api.undo(api.context) == MMS_OK && SameRegistrationKeys(fixture.first, before) && plugin.value == 0,
        "common undo visits the later other-model morph edit before the mixed plugin and bone edit");
    Check(api.redo(api.context) == MMS_OK && SameRegistrationKeys(fixture.first, after) && plugin.value == 42 &&
        api.redo(api.context) == MMS_OK && MorphKeysMatch(fixture.second, otherAfter), "common redo restores each original model and plugin state");
    Check(fixture.history.Reset() == MMS_OK, "retire model/plugin records before unregistering their serializer"); fixture.history.Collect();
    Check(fixture.storage.Unregister(storage, provider) == MMS_OK, "unregister model multiply regression provider");
}
Fixture* modalFixture = nullptr;
Kind modalKind = Kind::Bone;
bool modalCancel = false, modalIdentity = false;
unsigned modalCount = 0;
LRESULT CALLBACK ModalHidden(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) { auto* created = reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if (!(created->lpcs->style & WS_CHILD)) created->lpcs->x = created->lpcs->y = -32000; }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK ModalInitialized(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0) { const auto* message = reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (message->message == WM_INITDIALOG && modalFixture && GetWindow(message->hwnd, GW_OWNER) == modalFixture->app->MainWindow() &&
            GetDlgItem(message->hwnd, 686)) {
            ++modalCount;
            if (modalKind == Kind::Bone) modalFixture->Set(modalIdentity ? BoneMultiplyOptions{} : Fixture::BoneOptions(), message->hwnd);
            else modalFixture->Set(modalIdentity ? MorphMultiplyOptions{} : Fixture::MorphOptions(), message->hwnd);
            PostMessageW(message->hwnd, WM_COMMAND, modalCancel ? IDCANCEL : IDOK, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void RealMenu() {
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) for (auto kind : {Kind::Bone, Kind::Morph}) {
        Fixture fixture; NativePoseTestBinding binding(*fixture.app, fixture.history);
        Check(fixture.history.SetMode(mode) == MMS_OK, "set actual menu model multiply recording mode");
        Check(FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kind == Kind::Bone ? 0x294 : 0x295), MAKEINTRESOURCEW(5)) != nullptr,
            "actual product bone and morph dialog resources are linked");
        const auto beforeBone = RegistrationKeys(fixture.first); const auto beforeMorph = MorphKeysCopy(fixture.first);
        unsigned reads = 0;
        curve_editor::Source source; source.id = kind == Kind::Bone ? "bones" : "morph"; source.label = L"Multiply source";
        source.read = [&](std::vector<curve_editor::Channel>& out, std::wstring&) {
            ++reads; curve_editor::Channel channel; channel.id = "weight"; channel.label = L"Value";
            animation_curve::Key key; key.frame = 0; key.value = 0; channel.track.keys.push_back(key);
            out = {std::move(channel)}; return true;
        };
        curve_editor::Session session; session.id = "multiply-notification"; session.sources.push_back(std::move(source));
        const auto editor = static_cast<HWND>(curve_editor::Open(fixture.app->MainWindow(), std::move(session), false));
        struct CloseEditor { HWND owner; ~CloseEditor() { curve_editor::CloseOwned(owner); } } close{fixture.app->MainWindow()};
        Check(editor && reads, "open the actual matching bone or morph curve source before the multiply menu");
        const auto previousReads = reads;
        const auto previous = g_Block; g_Block = fixture.app.get();
        modalFixture = &fixture; modalKind = kind; modalCount = 0; modalCancel = modalIdentity = false;
        const auto hidden = SetWindowsHookExW(WH_CBT, ModalHidden, nullptr, GetCurrentThreadId());
        const auto initialized = SetWindowsHookExW(WH_CALLWNDPROCRET, ModalInitialized, nullptr, GetCurrentThreadId());
        Check(hidden && initialized, "observe and hide actual model multiply modal without replacing its procedure");
        const auto command = [&] { CmdViewMenu(fixture.app.get(), fixture.app->MainWindow(), kind == Kind::Bone ? 251 : 252, 0); };
        command();
        SendMessageW(editor, WM_APP + 401, 0, 0);
        Check(reads > previousReads && !fixture.app->PhysicsResetPending(),
            "actual multiply notifies its matching open curve source without resetting unrelated physical states");
        Check(modalCount == 1 && fixture.history.UndoCount() == 1 && fixture.app->SceneModified(),
            "actual 251 or 252 OK commits exactly one explicit node in either history mode");
        Check(fixture.history.History(false) == MMS_OK && SameRegistrationKeys(fixture.first, beforeBone) && MorphKeysMatch(fixture.first, beforeMorph),
            "real menu undo restores its source keys");
        const auto redo = fixture.history.RedoCount(); modalCancel = true; command();
        modalCancel = false; modalIdentity = true; command();
        Check(modalCount == 3 && fixture.history.RedoCount() == redo && fixture.history.UndoCount() == 0 &&
            SameRegistrationKeys(fixture.first, beforeBone) && MorphKeysMatch(fixture.first, beforeMorph),
            "actual modal cancel and identity OK both preserve existing redo and source payloads");
        UnhookWindowsHookEx(initialized); UnhookWindowsHookEx(hidden); modalFixture = nullptr; g_Block = previous;
    }
}
}
void NativeModelMultiplyChecks() {
    model_multiply_checks::ParityAndCrossModel(); model_multiply_checks::CleanEvaluation(); model_multiply_checks::Authors();
    model_multiply_checks::CancelNoopInvalidAndBudget(); model_multiply_checks::StaleAndAllocation();
    model_multiply_checks::PluginComposition(); model_multiply_checks::RealMenu();
}
