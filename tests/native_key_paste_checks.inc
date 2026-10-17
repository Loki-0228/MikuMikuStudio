// Include after native_bone_registration_checks.inc.
#include "../src/plugins/native_key_paste.hpp"
namespace mikudancestudio {
bool RegisterBoneKey(unsigned char*, unsigned char*, int, unsigned char);
bool RegisterMirroredBoneKey(unsigned char*, unsigned char*, int);
void ResetBoneKeyCursor(unsigned char*);
}
namespace {
struct KeyPasteFixture : BoneRegistrationFixture {
    std::array<mdl::BoneClipboardRecord, 3> clipboard{};
    KeyPasteFixture() {
        for (auto* f : {&first, &second}) {
            std::memcpy(f->bones[0].name, "\x8d\xb6 arm", 7);
            std::memcpy(f->bones[1].name, "\x89\x45 arm", 7);
            f->bones[0].type = f->bones[1].type = mdl::BoneType::RotateMove;
            f->bones[0].slotIndex = f->bones[1].slotIndex = -1;
        }
        for (unsigned i = 0; i < clipboard.size(); ++i) {
            auto& source = clipboard[i];
            std::memcpy(source.name, first.bones[0].name, sizeof(first.bones[0].name));
            source.frame = i * 10;
            source.rotation[0] = .1f; source.rotation[1] = .2f; source.rotation[2] = .3f; source.rotation[3] = .9f;
            source.position[0] = 2 + float(i); source.position[1] = -3; source.position[2] = 4;
            source.physicsDisabled = 1;
            for (int lane = 0; lane < 4; ++lane) {
                source.interpolation[lane] = 20; source.interpolation[lane + 4] = 30;
                source.interpolation[lane + 8] = 100; source.interpolation[lane + 12] = 110;
            }
            bone_scale::CopyClipboard(app.get(), int(i), Scale(2 + float(i)), false);
        }
        app->BoneClipboard() = clipboard.data(); app->ClipboardCounts().bones = 3; app->CurrentFrame() = 10;
        // The legacy registrar appends key snapshots into the caller-created
        // current undo entry. Production paste prepares this buffer first.
        auto& oracleUndo = second.m->undoRings[0].slots[second.m->undoState[0]];
        oracleUndo.auxiliaryPose = undo_storage::MallocOrThrow(32 * sizeof(mdl::BoneKeySnapshot));
        oracleUndo.dirty = 0;
        mdl::BoneKeyVisitClear(*second.m);
    }
    ~KeyPasteFixture() { app->BoneClipboard() = nullptr; app->ClipboardCounts() = {}; }
    std::shared_ptr<plugins::HistoryChange> Plan(bool mirror) {
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanBoneKeyPaste(*app, 0, mirror, change) == MMS_OK && change, "bone paste builds owned private plan");
        return change;
    }
};
void NativeKeyPasteChecks() {
    using Mode = plugins::SharedHistory::RecordingMode;
    std::puts("  key paste: legacy payload parity");
    for (bool mirror : {false, true}) for (int offset : {0, 10, 20, 40}) {
        KeyPasteFixture f; SeedRegistrationChain(f.first); SeedRegistrationChain(f.second);
        f.app->CurrentFrame() = offset;
        const auto before = RegistrationKeys(f.first); const auto ring = PoseRingDigest(f.first);
        const auto scales = bone_scale::AuthoringState::Capture(f.first.raw());
        const auto original = f.first.Digest(); auto change = f.Plan(mirror);
        Check(f.first.Digest() == original && bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scales),
            "paste planner does not mutate native pool selection pose or scale authoring");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("paste parity", change, transaction) == MMS_OK && f.first.Digest() == original,
            "paste preparation keeps live storage unchanged");
        Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK,
            "paste publication allocates nothing");
        for (unsigned index = 0; index < 32; ++index) f.second.m->boneKeys[index].allocated = 0;
        ResetBoneKeyCursor(f.second.raw());
        for (unsigned index = 0; index < f.clipboard.size(); ++index) {
            auto source = f.clipboard[index];
            bone_scale::Registration scale(bone_scale::Clipboard(f.app.get(), int(index), false, mirror),
                bone_scale::ClipboardCurve(f.app.get(), int(index), false));
            Check(mirror ? RegisterMirroredBoneKey(f.second.raw(), reinterpret_cast<unsigned char*>(&source), offset) :
                RegisterBoneKey(f.second.raw(), reinterpret_cast<unsigned char*>(&source), offset, 0), "legacy paste oracle registers key");
        }
        Check(!std::memcmp(f.first.m->boneKeys, f.second.m->boneKeys, 32 * sizeof(mdl::BoneKey)),
            "normal and mirrored paste match legacy payload links curves frame-zero overwrite and insertion");
        Check(f.first.m->maxFrame == f.second.m->maxFrame && PoseRingDigest(f.first) == ring &&
            f.history.UndoCount() == 1, "paste matches maximum frame and does not write legacy ring");
        for (unsigned index = 0; index < 32; ++index)
            Check(bone_scale::Key(f.first.raw(), int(index)) == bone_scale::Key(f.second.raw(), int(index)) &&
                bone_scale::KeyCurve(f.first.raw(), int(index)) == bone_scale::KeyCurve(f.second.raw(), int(index)),
                "paste owns legacy scale matrix and curve payload");
        const auto after = RegistrationKeys(f.first);
        f.clipboard = {}; f.app->ClipboardCounts() = {};
        Check(f.history.History(false) == MMS_OK && SameRegistrationKeys(f.first, before) &&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scales), "paste undo restores owned keys authoring and selection");
        Check(f.history.History(true) == MMS_OK && SameRegistrationKeys(f.first, after),
            "paste redo does not consult overwritten clipboard");
    }
    std::puts("  key paste: command modes and rejection");
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly}) {
        KeyPasteFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        Check(f.history.SetMode(mode) == MMS_OK, "paste history mode configured");
        CmdControl400(f.app.get(), f.app->MainWindow(), 421, 0);
        Check(f.history.UndoCount() == 1, "actual paste command records once under either history mode");
        Check(f.history.History(false) == MMS_OK, "paste route undo succeeds");
        const auto before = f.first.Digest(); const auto redo = f.history.RedoCount();
        f.app->ClipboardCounts().morphs = 1;
        CmdControl400(f.app.get(), f.app->MainWindow(), 421, 0);
        Check(f.first.Digest() == before && f.history.RedoCount() == redo,
            "mixed-family paste with a missing morph clipboard is rejected without fallback or lost redo");
        f.app->ClipboardCounts().morphs = 0; f.app->CurrentFrame() = frame_limits::maximum;
        CmdControl400(f.app.get(), f.app->MainWindow(), 421, 0);
        Check(f.first.Digest() == before && f.history.RedoCount() == redo,
            "overflow in a later copied key leaves all keys and redo intact");
        f.app->CurrentFrame() = 10;
        for (auto& source : f.clipboard) std::memcpy(source.name, "absent bone", 12);
        f.first.m->boneKeys[0].allocated = 1; f.first.m->morphKeys[0].allocated = 1;
        f.first.m->displayKeys[0].allocated = 1; f.first.m->searchCursor = 7;
        const auto unmatched = f.first.Digest();
        CmdControl400(f.app.get(), f.app->MainWindow(), 421, 0);
        Check(f.first.Digest() == unmatched && f.history.RedoCount() == redo,
            "entirely unmatched paste keeps marks nondefault cursor and existing redo");
    }
    std::puts("  key paste: fixed-axis legacy parity");
    for (bool mirror : {false, true}) for (unsigned char physics : {1, 2}) for (bool perpendicular : {false, true}) {
        KeyPasteFixture f;
        Check(d3dx::Get().Load(), "D3DX loaded for independent fixed-axis paste parity");
        if (perpendicular) for (auto& source : f.clipboard) {
            source.rotation[0] = .6f; source.rotation[1] = source.rotation[2] = 0; source.rotation[3] = .8f;
        }
        for (auto* model : {&f.first, &f.second}) {
            model->m->physicsMode = physics;
            for (unsigned bone = 0; bone < 2; ++bone) {
                model->bones[bone].type = mdl::BoneType::FixedAxis;
                model->bones[bone].tailBone = int(1 - bone);
                model->bones[bone].axis[0] = .26726124f; model->bones[bone].axis[1] = .53452248f;
                model->bones[bone].axis[2] = .80178373f;
                if (bone) { model->bones[bone].position[0] = 1; model->bones[bone].position[1] = 2; model->bones[bone].position[2] = 3; }
                if (perpendicular) {
                    model->bones[bone].axis[0] = model->bones[bone].axis[2] = 0; model->bones[bone].axis[1] = 1;
                    model->bones[bone].position[0] = model->bones[bone].position[2] = 0;
                }
            }
        }
        auto change = f.Plan(mirror); std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("fixed axis paste", change, transaction) == MMS_OK &&
            CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK,
            "owned fixed-axis paste publishes prepared correction");
        ResetBoneKeyCursor(f.second.raw());
        for (unsigned index = 0; index < f.clipboard.size(); ++index) {
            auto source = f.clipboard[index];
            bone_scale::Registration scale(bone_scale::Clipboard(f.app.get(), int(index), false, mirror),
                bone_scale::ClipboardCurve(f.app.get(), int(index), false));
            Check(mirror ? RegisterMirroredBoneKey(f.second.raw(), reinterpret_cast<unsigned char*>(&source), 10) :
                RegisterBoneKey(f.second.raw(), reinterpret_cast<unsigned char*>(&source), 10, 0), "legacy fixed-axis oracle pastes");
        }
        Check(!std::memcmp(f.first.m->boneKeys, f.second.m->boneKeys, 32 * sizeof(mdl::BoneKey)),
            "normal and mirrored fixed-axis paste matches PMX arbitrary axis and PMD normalized tail axis");
    }
    {
        KeyPasteFixture f; const auto before = f.first.Digest(); const auto ring = PoseRingDigest(f.first);
        auto change = f.Plan(false); std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("cancel paste", change, tx) == MMS_OK, "paste candidate prepares before cancellation");
        tx.reset();
        Check(f.first.Digest() == before && f.history.UndoCount() == 0, "cancelled paste keeps authored state untouched");
        Check(f.history.PrepareNative("stale paste", change, tx) == MMS_OK, "paste candidate prepares for identity recheck");
        f.first.bones[0].name[0] = 'x';
        Check(f.history.CommitNative(std::move(tx)) == MMS_E_STALE && PoseRingDigest(f.first) == ring && !f.history.UndoCount(),
            "destination name change invalidates prepared paste without legacy history write");
    }
    {
        KeyPasteFixture f;
        f.first.m->boneKeys[0].next = 2;
        for (unsigned index = 2; index < 32; ++index) {
            auto& key = f.first.m->boneKeys[index]; key.frame = index - 1;
            key.previous = index == 2 ? 0 : index - 1; key.next = index == 31 ? 0 : index + 1;
            key.rotation[3] = 1;
        }
        f.app->CurrentFrame() = 100;
        const auto original = RegistrationKeys(f.first); const auto identity = mdl::BoneKeyPoolIdentity(f.first.m->boneKeys);
        const auto capacity = mdl::BoneKeyCapacity(f.first.m->boneKeys); auto* pool = f.first.m->boneKeys;
        auto change = f.Plan(false); std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("growth paste", change, tx) == MMS_OK && f.first.m->boneKeys == pool &&
            mdl::BoneKeyCapacity(pool) == capacity, "full-pool paste prepares growth entirely off-scene");
        Check(CommitRegistrationNoAllocation(f.history, std::move(tx)) == MMS_OK &&
            mdl::BoneKeyCapacity(f.first.m->boneKeys) >= capacity + 3 && mdl::BoneKeyPoolIdentity(f.first.m->boneKeys) == identity,
            "full-pool paste publishes complete growth without replacing ownership identity");
        Check(f.history.History(false) == MMS_OK && SameRegistrationKeys(f.first, original),
            "grown paste undo restores original pool contents without shrinking");
        Check(f.history.History(true) == MMS_OK && f.first.m->boneKeys[31].next >= capacity,
            "grown paste redo restores links into retained capacity");
    }
    {
        KeyPasteFixture f;
        // Legacy frame-zero merges may leave links in unreachable free slots.
        // A new append must terminate its own chain rather than reuse them.
        f.first.m->boneKeys[2].next = 1;
        const auto original = RegistrationKeys(f.first); auto change = f.Plan(false);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("free slot paste", change, tx) == MMS_OK &&
            CommitRegistrationNoAllocation(f.history, std::move(tx)) == MMS_OK,
            "paste can reuse a free slot with a stale outgoing link");
        Check(f.first.m->boneKeys[0].next == 2 && f.first.m->boneKeys[4].frame == 30 && !f.first.m->boneKeys[4].next,
            "new append clears stale next before reconstructing the source chain");
        Check(f.history.History(false) == MMS_OK && SameRegistrationKeys(f.first, original),
            "undo restores even the original unreachable free-slot bytes");
    }
    {
        KeyPasteFixture f; const auto before = f.first.Digest();
        for (long failure = 0; failure < 24; ++failure) {
            std::shared_ptr<plugins::HistoryChange> change; failAfter = failure;
            const auto result = plugins::PlanBoneKeyPaste(*f.app, 0, false, change); failAfter = -1;
            Check((result == MMS_E_MEMORY || result == MMS_OK) && f.first.Digest() == before && !f.history.UndoCount(),
                "private paste allocation failures never publish partial keys");
        }
        Check(f.history.Configure(Mode::AllOperations, 32, 1) == MMS_OK, "paste tiny budget configured");
        auto change = f.Plan(false); std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("budget paste", change, tx) != MMS_OK && f.first.Digest() == before,
            "paste budget rejection occurs before live writes");
    }
}
}
