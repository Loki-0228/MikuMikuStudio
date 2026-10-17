// Include after native_bone_registration_checks.inc. The unbound command is
// the authoring oracle; both commands use real hidden Win32 interpolation UI.
#include "../src/plugins/native_key_commands.hpp"
namespace {
struct BoneInterpolationFixture : BoneRegistrationFixture {
    HDC curveDc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    HWND combo = nullptr;
    BoneInterpolationFixture() {
        app->CurrentFrame() = 17; app->state.lastRegisteredFrame = 88;
        app->PhysicsResetPending() = 0;
        combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
            0, 0, 80, 120, app->MainWindow(), reinterpret_cast<HMENU>(INT_PTR(panel::kInterpCurveCombo)),
            GetModuleHandleW(nullptr), nullptr);
        Check(combo != nullptr, "hidden interpolation combo created");
        for (int channel = 0; channel < 6; ++channel)
            Check(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"channel")) == channel,
                  "hidden interpolation combo has the model six-channel layout");
        for (int id : {430, 431, 432})
            Check(CreateWindowExW(0, L"BUTTON", L"curve", WS_CHILD, 0, 0, 40, 20, app->MainWindow(),
                reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr) != nullptr,
                "hidden interpolation action button created");
        curveDc = CreateCompatibleDC(nullptr); bitmap = CreateBitmap(128, 128, 1, 32, nullptr);
        Check(curveDc && bitmap, "hidden interpolation drawing surface created");
        previousBitmap = SelectObject(curveDc, bitmap); app->CurveDC() = curveDc;
        for (auto* fixture : {&first, &second}) {
            SeedRegistrationChain(*fixture);
            for (unsigned slot = 0; slot < 4; ++slot) {
                auto& key = fixture->m->boneKeys[slot]; key.allocated = slot == 0 ? 2 : slot == 2 ? 3 : 0;
                for (unsigned lane = 0; lane < 4; ++lane) {
                    key.interpolation[lane] = std::uint8_t(8 + lane);
                    key.interpolation[lane + 4] = std::uint8_t(17 + slot);
                    key.interpolation[lane + 8] = std::uint8_t(95 + lane);
                    key.interpolation[lane + 12] = std::uint8_t(120 - slot);
                }
                bone_scale::SetKeyCurve(fixture->raw(), int(slot), {9, 31, 101, 119});
            }
            fixture->dirty[0] = 0; fixture->dirty[1] = 1;
            fixture->bones[1].trans[2] = 71;
            fixture->m->morphKeys[0].allocated = 4; fixture->m->displayKeys[0].allocated = 5;
        }
        for (unsigned lane = 0; lane < 5; ++lane) {
            app->state.lightA[lane] = std::uint8_t(12 + lane); app->state.lightB[lane] = std::uint8_t(30 + lane);
            app->state.lightC[lane] = std::uint8_t(80 + lane); app->state.lightD[lane] = std::uint8_t(110 + lane);
        }
        Channel(0);
    }
    ~BoneInterpolationFixture() {
        app->CurveDC() = nullptr;
        SelectObject(curveDc, previousBitmap); DeleteObject(bitmap); DeleteDC(curveDc);
    }
    void Channel(int channel) { SendMessageW(combo, CB_SETCURSEL, WPARAM(channel), 0); }
    void Command(bool reset) { CmdControl400(app.get(), app->MainWindow(), reset ? 432 : 431, 0); }
    plugins::BoneInterpolationOptions Options(int channel, bool reset = false) const {
        plugins::BoneInterpolationOptions options;
        options.channels = channel == 5 ? 31 : std::uint8_t(1u << channel);
        for (unsigned lane = 0; lane < 5; ++lane)
            options.values[lane] = reset ? bone_scale::LinearCurve() : bone_scale::Curve{
                app->state.lightA[lane], app->state.lightB[lane], app->state.lightC[lane], app->state.lightD[lane]};
        return options;
    }
};
struct InterpolationPoseState {
    std::array<mdl::BoneRecord, 2> bones;
    std::array<unsigned char, 2> dirty;
    bone_scale::Values scale;
    explicit InterpolationPoseState(Fixture& f) : bones(f.bones), dirty(f.dirty), scale(bone_scale::PoseValues(f.raw())) {}
    bool Matches(Fixture& f) const {
        for (unsigned bone = 0; bone < bones.size(); ++bone)
            if (std::memcmp(bones[bone].trans, f.bones[bone].trans, sizeof(bones[bone].trans)) ||
                std::memcmp(bones[bone].rotQuat, f.bones[bone].rotQuat, sizeof(bones[bone].rotQuat)) ||
                bones[bone].physicsDisabled != f.bones[bone].physicsDisabled) return false;
        return dirty == f.dirty && bone_scale::PoseValuesEqual(f.raw(), scale);
    }
};
std::shared_ptr<plugins::HistoryChange> InterpolationPlan(BoneInterpolationFixture& f,
    const plugins::BoneInterpolationOptions& options, unsigned slot = 0) {
    std::shared_ptr<plugins::HistoryChange> result;
    Check(plugins::PlanBoneInterpolationEdit(*f.app, slot, options, result) == MMS_OK,
          "interpolation plans its complete private authoring state");
    return result;
}
void InterpolationCommit(BoneInterpolationFixture& f, const std::shared_ptr<plugins::HistoryChange>& change) {
    Check(bool(change), "interpolation owns an actual authoring change");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("bone curves", change, transaction) == MMS_OK,
          "interpolation prepares all curve participants");
    Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK,
          "interpolation publishes with allocation disabled");
}
void InterpolationLegacyAndRoutes() {
    for (bool reset : {false, true}) for (int channel = 0; channel < 6; ++channel) for (bool linked : {false, true}) {
        std::printf("  interpolation reset=%u channel=%d link=%u\n", unsigned(reset), channel, unsigned(linked));
        BoneInterpolationFixture f; f.Channel(channel); f.Automatic(linked);
        auto keys = RegistrationKeys(f.first); const auto ring = PoseRingDigest(f.first);
        const auto scales = bone_scale::AuthoringState::Capture(f.first.raw());
        const InterpolationPoseState pose(f.first);
        const auto maximum = f.first.m->maxFrame; const auto cursor = f.first.m->searchCursor;
        Check(!plugins::TryApplyBoundInterpolation(f.app.get(), f.app->MainWindow(), reset),
              "unbound interpolation helper preserves the legacy command owner");
        f.app->SelectedModelSlot() = 1; f.Command(reset); f.app->SelectedModelSlot() = 0;
        Check(f.second.m->undoState[0] == 2 && f.history.UndoCount() == 0,
              "unbound interpolation command still records its legacy ring");
        f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0;
        plugins::NativePoseTestBinding binding(*f.app, f.history); f.Command(reset);
        Check(SameRegistrationKeys(f.first, RegistrationKeys(f.second)),
              "actual interpolation command matches legacy native bytes for single and all channels");
        for (unsigned slot = 0; slot < 32; ++slot)
            Check(bone_scale::KeyCurve(f.first.raw(), int(slot)) == bone_scale::KeyCurve(f.second.raw(), int(slot)) &&
                  bone_scale::Key(f.first.raw(), int(slot)) == bone_scale::Key(f.second.raw(), int(slot)),
                  "actual interpolation command matches legacy scale curves without changing scale keys");
        Check(pose.Matches(f.first) && PoseRingDigest(f.first) == ring && f.SelectionUnchanged() &&
              f.first.m->morphKeys[0].allocated == 4 && f.first.m->displayKeys[0].allocated == 5 &&
              f.first.m->maxFrame == maximum && f.first.m->searchCursor == cursor &&
              f.app->state.lastRegisteredFrame == 88 && f.app->CurrentFrame() == 17 && !f.app->PhysicsResetPending() &&
              f.history.UndoCount() == 1 && f.app->SceneModified() == 1 && f.app->RecordedFrameCountByte1() == 1,
              "interpolation changes one common node and dirty UI flags without seek pose physics metadata marks or old-ring writes");
        auto after = RegistrationKeys(f.first); const auto scaleAfter = bone_scale::AuthoringState::Capture(f.first.raw());
        for (unsigned slot : {0u, 2u, 3u}) {
            const unsigned char mark = slot == 0 ? 7 : slot == 3 ? 4 : 0;
            f.first.m->boneKeys[slot].allocated = keys[slot].allocated = after[slot].allocated = mark;
        }
        f.first.m->morphKeys[0].allocated = 9; f.first.m->displayKeys[0].allocated = 10;
        f.first.bones[0].trans[0] = 843; f.first.dirty[0] = 1; bone_scale::SetPose(f.first.raw(), 1, Scale(7));
        const InterpolationPoseState currentPose(f.first);
        f.first.m->maxFrame = 903; f.first.m->searchCursor = 19; f.app->state.lastRegisteredFrame = 904;
        f.app->CurrentFrame() = 26;
        Check(f.history.History(false) == MMS_OK && SameRegistrationKeys(f.first, keys) &&
              bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scales) && currentPose.Matches(f.first),
              "interpolation undo restores authoring while preserving later marks and unregistered pose at another frame");
        f.app->CurrentFrame() = 35;
        Check(f.history.History(true) == MMS_OK && SameRegistrationKeys(f.first, after) &&
              bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleAfter) && currentPose.Matches(f.first) &&
              f.first.m->morphKeys[0].allocated == 9 && f.first.m->displayKeys[0].allocated == 10 &&
              f.first.m->maxFrame == 903 && f.first.m->searchCursor == 19 && f.app->state.lastRegisteredFrame == 904 &&
              f.app->CurrentFrame() == 35 && !f.app->PhysicsResetPending(),
              "interpolation redo does not seek or restore stale marks pose and timeline metadata");
    }
}
void InterpolationFreeCurvesAndGrowth() {
    {
        BoneInterpolationFixture f; std::string error;
        for (int bone = 0; bone < 2; ++bone)
            Check(bone_curve::Write(f.first.raw(), bone, RegistrationFreeTracks(), error), "free interpolation owner seeded");
        for (std::size_t slot = 0; slot < mdl::BoneKeyCapacity(f.first.m->boneKeys); ++slot)
            f.first.m->boneKeys[slot].allocated = 0;
        const auto selected = f.first.m->boneKeys[0].next;
        Check(selected >= f.first.m->boneCount, "free curve owns a non-root key");
        f.first.m->boneKeys[selected].allocated = 3;
        const auto keys = RegistrationKeys(f.first); const auto authored = bone_curve::AuthoringState::Capture(f.first.raw());
        const auto scale = bone_scale::AuthoringState::Capture(f.first.raw()); const InterpolationPoseState pose(f.first);
        auto options = f.Options(4); options.values[4] = bone_scale::KeyCurve(f.first.raw(), int(selected));
        Check(!InterpolationPlan(f, options) && bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*authored),
              "equal scale interpolation keeps both free-curve owners intact");
        options.values[4] = {11, 34, 91, 123};
        auto change = InterpolationPlan(f, options);
        Check(SameRegistrationKeys(f.first, keys) && bone_curve::Has(f.first.raw(), 0) && bone_curve::Has(f.first.raw(), 1),
              "scale-only interpolation planning does not invalidate live free curves");
        InterpolationCommit(f, change);
        bone_curve::Tracks preserved,other;
        Check(SameRegistrationKeys(f.first, keys) && bone_curve::Read(f.first.raw(),0,preserved) && bone_curve::Read(f.first.raw(),1,other) && pose.Matches(f.first)&&
              preserved[0].keys.front().outWeight==.2&&preserved[0].keys.back().inWeight==.4&&other[0].keys.front().outWeight==.2&&
              std::abs(preserved[6].keys.front().outWeight-11.0/127)<1e-12&&std::abs(preserved[6].keys.back().inWeight-(1-91.0/127))<1e-12,
              "scale-only interpolation rebuilds its scale handles while preserving translation handles, other authored bone and current pose");
        const auto reconciled=bone_curve::AuthoringState::Capture(f.first.raw());
        Check(f.history.History(false) == MMS_OK && bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale) &&
              bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*authored) && bone_curve::Has(f.first.raw(), 0),
              "scale-only undo restores the free curve and its matching native hash");
        Check(f.history.History(true) == MMS_OK && bone_curve::Has(f.first.raw(), 0) && bone_curve::Has(f.first.raw(), 1)&&
              bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*reconciled),
              "scale-only redo restores the exact rebuilt authoring while retaining both free-curve owners");
    }
    {
        BoneInterpolationFixture f; const auto before = RegistrationKeys(f.first);
        const auto identity = mdl::BoneKeyPoolIdentity(f.first.m->boneKeys);
        InterpolationCommit(f, InterpolationPlan(f, f.Options(5))); const auto after = RegistrationKeys(f.first);
        Check(mdl::BoneKeyPoolEnsure(*f.first.m, 65) && mdl::BoneKeyPoolIdentity(f.first.m->boneKeys) == identity,
              "interpolation fixture grows its registered pool without replacing its identity");
        f.first.m->boneKeys[64].position[2] = 93; f.first.m->boneKeys[64].reserved[0] = 71;
        const auto extra = f.first.m->boneKeys[64]; const auto* pool = f.first.m->boneKeys;
        Check(f.history.History(false) == MMS_OK && SameRegistrationKeys(f.first, before) && f.first.m->boneKeys == pool &&
              !std::memcmp(&f.first.m->boneKeys[64], &extra, sizeof(extra)),
              "interpolation undo after normal growth preserves the grown pool and unrelated new slots");
        Check(f.history.History(true) == MMS_OK && SameRegistrationKeys(f.first, after) &&
              !std::memcmp(&f.first.m->boneKeys[64], &extra, sizeof(extra)),
              "interpolation redo resolves original slots after normal pool growth");
    }
    {
        BoneInterpolationFixture f; const auto first = RegistrationKeys(f.first), second = RegistrationKeys(f.second);
        const auto secondScale = bone_scale::AuthoringState::Capture(f.second.raw());
        InterpolationCommit(f, InterpolationPlan(f, f.Options(0), 0));
        InterpolationCommit(f, InterpolationPlan(f, f.Options(4), 1));
        Check(f.history.UndoCount() == 2 && f.history.History(false) == MMS_OK && SameRegistrationKeys(f.second, second) &&
              bone_scale::AuthoringState::Capture(f.second.raw()).Equals(secondScale) &&
              f.history.History(false) == MMS_OK && SameRegistrationKeys(f.first, first),
              "interpolation edits on two models follow one common history");
    }
}
void InterpolationNoopsAndRejections() {
    BoneInterpolationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
    f.Commit(panel::kReadoutPosXEdit, "5"); Check(f.history.History(false) == MMS_OK, "seed common redo for interpolation rejection");
    f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0; f.app->PhysicsResetPending() = 0;
    for (std::size_t slot = 0; slot < mdl::BoneKeyCapacity(f.first.m->boneKeys); ++slot) f.first.m->boneKeys[slot].allocated = 0;
    auto unchanged = f.first.Digest(); f.Command(false); f.Command(true);
    Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified(),
          "zero-selection interpolation commands preserve redo and all live state");
    f.first.m->boneKeys[2].allocated = 3; f.Channel(0);
    const auto& key = f.first.m->boneKeys[2];
    f.app->state.lightA[0] = key.interpolation[0]; f.app->state.lightB[0] = key.interpolation[4];
    f.app->state.lightC[0] = key.interpolation[8]; f.app->state.lightD[0] = key.interpolation[12];
    unchanged = f.first.Digest(); f.Command(false);
    Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified(),
          "equal interpolation preserves redo without writing old rings");
    for (unsigned point = 0; point < 4; ++point)
        f.first.m->boneKeys[2].interpolation[point * 4] = bone_scale::LinearCurve()[point];
    unchanged = f.first.Digest(); f.Command(true);
    Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified(),
          "resetting an already linear channel preserves redo without dirtying the scene");
    f.app->state.lightA[0] = 15;
    for (int channel : {-1, 6}) {
        if (channel == 6) SendMessageW(f.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"invalid"));
        f.Channel(channel); f.Command(false); f.Command(true);
        Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified(),
              "negative and excessive combo indices reject before dirty or legacy writes");
    }
    f.Channel(0); f.app->state.lightA[0] = 128; f.Command(false); f.app->state.lightA[0] = 15;
    Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified(),
          "interpolation handles above 127 reject without partial mutation");
    for (int gate = 0; gate < 6; ++gate) {
        if (gate == 0) f.app->CameraMode() = 1;
        if (gate == 1) f.app->PlaybackActive() = 1;
        if (gate == 2) f.app->FrameStepPlayback() = 1;
        if (gate == 3) f.app->state.recordPlaybackStartPending = 1;
        if (gate == 4) f.app->SelectedModelSlot() = 255;
        if (gate == 5) Check(f.history.Enter() == MMS_OK, "hold interpolation history gate");
        f.Command(false); f.Command(true);
        if (gate == 5) f.history.Leave();
        f.app->CameraMode() = f.app->PlaybackActive() = f.app->FrameStepPlayback() = 0;
        f.app->state.recordPlaybackStartPending = 0; f.app->SelectedModelSlot() = 0;
        Check(f.first.Digest() == unchanged && f.history.RedoCount() == 1 && !f.app->SceneModified() &&
              !f.app->RecordedFrameCountByte1() && !f.app->PhysicsResetPending(),
              "bound camera playback recording invalid-model and busy commands never fall through");
    }
    auto invalid = f.Options(0); invalid.channels = 32; std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanBoneInterpolationEdit(*f.app, 0, invalid, change) == MMS_E_ARGUMENT && !change,
          "interpolation planner rejects unknown channel bits");
    f.first.m->boneKeys[10].allocated = 1; const auto orphan = f.first.Digest();
    Check(plugins::PlanBoneInterpolationEdit(*f.app, 0, f.Options(0), change) == MMS_E_ARGUMENT && !change &&
          f.first.Digest() == orphan, "interpolation refuses a marked orphan slot");
}
void InterpolationFailureAndStale() {
    {
        BoneInterpolationFixture f; const auto options = f.Options(5); const auto before = f.first.Digest();
        const auto scale = bone_scale::AuthoringState::Capture(f.first.raw());
        const auto curves = bone_curve::AuthoringState::Capture(f.first.raw());
        std::shared_ptr<plugins::HistoryChange> change; bool planned = false; unsigned failed = 0;
        for (long limit = 0; limit < 512 && !planned; ++limit) {
            failAfter = limit; const auto result = plugins::PlanBoneInterpolationEdit(*f.app, 0, options, change); failAfter = -1;
            Check(f.first.Digest() == before && bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale) &&
                  bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*curves),
                  "interpolation planning allocation failure leaves authoring and native state untouched");
            if (result == MMS_OK) { planned = true; Check(bool(change), "interpolation fault sweep obtains a change"); }
            else { ++failed; Check(result == MMS_E_MEMORY && !change, "failed interpolation plan has no partial output"); }
        }
        Check(planned && failed > 8, "interpolation planning sweep covers sparse keys and both authoring snapshots");
        bool prepared = false; failed = 0; std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        for (long limit = 0; limit < 512 && !prepared; ++limit) {
            std::string label = "interpolation allocation";
            failAfter = limit; const auto result = f.history.PrepareNative(std::move(label), change, transaction); failAfter = -1;
            Check(f.first.Digest() == before && bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale),
                  "interpolation preparation allocation failure cannot publish any curve participant");
            if (result == MMS_OK) prepared = true;
            else { ++failed; Check(result == MMS_E_MEMORY && !transaction && !f.history.UndoCount(),
                                   "failed interpolation preparation leaves no transaction or history node"); }
        }
        Check(prepared && failed > 5 && CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_OK,
              "interpolation fault sweep completes with no-allocation publication");
        const auto after = f.first.Digest();
        failAfter = 0; const auto result = f.history.History(false); failAfter = -1;
        Check(result == MMS_E_MEMORY && f.first.Digest() == after && f.history.UndoCount() == 1,
              "failed interpolation undo preparation preserves the committed state");
        for (bool forward : {false, true}) {
            std::unique_ptr<plugins::PreparedHistoryChange> candidate;
            Check(change->Prepare(forward, candidate) == MMS_OK && candidate->IsCurrent(),
                  "interpolation inverse and redo candidates prepare privately");
            failAfter = 0; candidate->Publish(); failAfter = -1;
            Check(f.first.Digest() == (forward ? after : before),
                  "interpolation inverse and redo publication allocate nothing and preserve pose metadata and old rings");
        }
    }
    for (int mutation = 0; mutation < 5; ++mutation) {
        BoneInterpolationFixture f; auto change = InterpolationPlan(f, f.Options(5));
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("interpolation stale", change, transaction) == MMS_OK, "prepare stale interpolation fixture");
        auto* original = f.first.m->boneKeys; mdl::BoneKey* replacement = nullptr;
        if (mutation == 0) f.first.m->boneKeys[2].interpolation[0] ^= 1;
        if (mutation == 1) bone_scale::SetKeyCurve(f.first.raw(), 2, {17, 27, 90, 115});
        if (mutation == 2) {
            const auto old = bone_scale::KeyCurve(f.first.raw(), 2);
            bone_scale::SetKeyCurve(f.first.raw(), 2, {17, 27, 90, 115}); bone_scale::SetKeyCurve(f.first.raw(), 2, old);
        }
        if (mutation == 3) {
            replacement = mdl::BoneKeyPoolCreate(32); Check(replacement != nullptr, "replacement interpolation pool allocated");
            std::memcpy(replacement, original, 32 * sizeof(mdl::BoneKey)); f.first.m->boneKeys = replacement;
        }
        if (mutation == 4) f.first.m->boneKeys[2].allocated ^= 1;
        const auto stale = f.first.Digest();
        Check(CommitRegistrationNoAllocation(f.history, std::move(transaction)) == MMS_E_STALE &&
              f.first.Digest() == stale && !f.history.UndoCount(),
              "interpolation rejects changed native curves side tables ABA revisions equal replacement pools and late marks");
        if (replacement) { f.first.m->boneKeys = original; mdl::BoneKeyPoolDestroy(replacement); }
    }
    {
        BoneInterpolationFixture f; plugins::NativePoseTestBinding binding(*f.app, f.history);
        f.Commit(panel::kReadoutPosXEdit, "5"); Check(f.history.History(false) == MMS_OK, "seed redo for actual interpolation failures");
        f.Channel(5); f.app->SceneModified() = f.app->RecordedFrameCountByte1() = 0; f.app->PhysicsResetPending() = 0;
        const auto before = f.first.Digest(); const auto scale = bone_scale::AuthoringState::Capture(f.first.raw());
        bool committed = false; unsigned failed = 0;
        for (long limit = 0; limit < 512 && !committed; ++limit) {
            failAfter = limit; f.Command(false); failAfter = -1;
            if (f.history.UndoCount()) committed = true;
            else { ++failed; Check(f.first.Digest() == before && bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale) &&
                    f.history.RedoCount() == 1 && !f.app->SceneModified() && !f.app->RecordedFrameCountByte1() &&
                    !f.app->PhysicsResetPending() && f.SelectionUnchanged(),
                    "actual interpolation allocation failure preserves authoring redo selection and dirty metadata"); }
        }
        Check(committed && failed > 10 && f.history.UndoCount() == 1 && !f.history.RedoCount() &&
              f.app->SceneModified() == 1 && !f.app->PhysicsResetPending(),
              "actual interpolation command branches redo once after all participants are prepared");
    }
}
void NativeBoneInterpolationChecks() {
    std::puts("RUN bone interpolation legacy and real routes"); InterpolationLegacyAndRoutes();
    std::puts("RUN bone interpolation free curves pool growth and models"); InterpolationFreeCurvesAndGrowth();
    std::puts("RUN bone interpolation no-op and rejection"); InterpolationNoopsAndRejections();
    std::puts("RUN bone interpolation failures stale and allocation-free publication"); InterpolationFailureAndStale();
}
}
