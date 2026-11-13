// Include after native_signed_delete_model_command_checks.inc with its existing
// prerequisites. May share the TU with native_signed_delete_command_checks.inc.
// Entry: NativeSignedDeleteContextChecks(). Uses real CmdControl400 replay.
// Not covered: global deletion replay in model mode, prepared-candidate binding
// replacement/refusal, physical keyboard dispatch, selector-window regressions,
// removed/replaced models, dirty-pose/physics rigs, or allocation/budget failure.

namespace native_signed_delete_context_checks {
using namespace mikumikustudio;
namespace command = native_signed_delete_model_command_checks;
namespace target = model_delete_target_checks;
namespace negative = negative_model_delete_target_checks;
using Fixture = command::Fixture;
using Mode = plugins::SharedHistory::RecordingMode;

// Digest alone does not include the pointed-to morphs, IK, order, selection,
// or authoring side tables. Keep those too when asserting that B is untouched.
struct PeerState {
    std::uint64_t digest, ring;
    ModelKeyState keys;
    std::array<mdl::MorphRecord, 1> morphs;
    std::array<unsigned char, 2> selected;
    mdl::IkChain ik;
    mdl::BoneOrderEntry order;
    bone_scale::Values scales;
    bone_scale::AuthoringState scaleAuthors;
    std::unique_ptr<bone_curve::AuthoringState> boneAuthors;
    morph_curve::AuthoredLayers morphAuthors;
    explicit PeerState(Fixture& f) : digest(f.second.Digest()), ring(PoseRingDigest(f.second)),
        keys(f.second), morphs(f.second.morphs), selected(f.second.selected),
        ik(f.secondIk[0]), order(f.secondOrder[0]), scales(bone_scale::PoseValues(f.second.raw())),
        scaleAuthors(bone_scale::AuthoringState::Capture(f.second.raw())),
        boneAuthors(bone_curve::AuthoringState::Capture(f.second.raw())) {
        std::string error;
        Check(boneAuthors && morph_curve::CaptureLayers(f.second.raw(), morphAuthors, error),
            "capture complete peer authoring before context replay");
    }
    bool Matches(Fixture& f) const {
        const auto currentBones = bone_curve::AuthoringState::Capture(f.second.raw());
        return digest == f.second.Digest() && ring == PoseRingDigest(f.second) && keys.Matches(f.second) &&
            !std::memcmp(morphs.data(), f.second.morphs.data(), sizeof(morphs)) && selected == f.second.selected &&
            !std::memcmp(&ik, &f.secondIk[0], sizeof(ik)) &&
            !std::memcmp(&order, &f.secondOrder[0], sizeof(order)) &&
            bone_scale::PoseValuesEqual(f.second.raw(), scales) &&
            scaleAuthors.Equals(bone_scale::AuthoringState::Capture(f.second.raw())) &&
            currentBones && boneAuthors->Equals(*currentBones) &&
            morph_curve::LayersUnchanged(f.second.raw(), morphAuthors);
    }
};

struct Selection {
    unsigned char camera, slot;
    int frame, firstBone, secondBone;
    std::array<unsigned char, 2> first, second;
    std::array<unsigned char, 4> global;
    unsigned char accessory;
    explicit Selection(Fixture& f) : camera(f.app->CameraMode()), slot(f.app->SelectedModelSlot()),
        frame(f.app->CurrentFrame()), firstBone(f.first.m->selectedBone), secondBone(f.second.m->selectedBone),
        first(f.first.selected), second(f.second.selected), accessory(f.accessory.rowSelected) {
        std::copy_n(f.app->state.globalTrackSelected, global.size(), global.begin());
    }
    bool Matches(Fixture& f) const {
        return camera == f.app->CameraMode() && slot == f.app->SelectedModelSlot() &&
            frame == f.app->CurrentFrame() && firstBone == f.first.m->selectedBone &&
            secondBone == f.second.m->selectedBone && first == f.first.selected && second == f.second.selected &&
            std::equal(global.begin(), global.end(), f.app->state.globalTrackSelected) &&
            accessory == f.accessory.rowSelected;
    }
};

void Seed(Fixture& f, bool display, bool automatic, int frame) {
    command::Seed(f, display, automatic, command::Selection::Mixed);
    negative::Owner owner(f.app.get());
    // B has its own marked positive keys and a distinguishable current bone.
    // Replaying A must neither consume B's marks nor change B's live pose.
    f.second.m->boneKeys[2].allocated = 3;
    f.second.m->morphKeys[1].allocated = 3;
    f.second.m->displayKeys[1].allocated = 3;
    f.second.m->selectedBone = 1;
    f.second.selected = {0, 1};
    command::coordinator::Frame(f, frame);
    Check(!plugins::BoundNativePoseHistory(f.app.get()),
        "context fixture finishes all seed writes without installing an oracle binding");
}

void SelectContext(Fixture& f, bool camera, int frame) {
    // Change timeline context directly; this is not a model-combo/window test.
    // Seek only A through the real evaluator, just as the existing command
    // oracle does, so B is an exact untouched peer across all replay frames.
    f.app->SelectedModelSlot() = 1;
    f.app->CameraMode() = camera ? 1 : 0;
    f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Gravity);
    f.accessory.rowSelected = 0;
    command::coordinator::Frame(f, frame);
}

void ModelReplay(Mode mode, bool automatic, bool display, int initialFrame) {
    std::printf("  signed delete context mode=%d automatic=%u display=%u initial=%d\n",
        int(mode), unsigned(automatic), unsigned(display), initialFrame);
    std::fflush(stdout);
    Fixture reference;
    Seed(reference, display, automatic, initialFrame);
    Fixture f;
    Seed(f, display, automatic, initialFrame);
    Check(f.history.SetMode(mode) == MMS_OK, "choose context replay recording mode");
    command::Oracle oracle(reference, automatic, false, true, true);
    const ModelKeyState original(f.first);
    const target::Authors originalAuthors(f);
    const auto originalNegative = NegativeTestRecord(*f.app, negative_frames::kPmmKey);
    const PeerState peer(f);
    const auto ring = PoseRingDigest(f.first);
    const auto unselected = command::UnselectedNegativeSources(f);
    oracle.Publish(true, true);

    negative::Owner owner(f.app.get());
    // This is the sole binding for the entire case. The independent fixture
    // never binds, including inside the typed oracle's directional replay.
    plugins::NativePoseTestBinding binding(*f.app, f.history);
    const auto generation = plugins::BoundNativePoseGeneration(f.app.get());
    Check(generation != 0, "actual context replay starts with a live binding generation");
    command::Delete(f, command::Route::Menu423);
    command::HistoryPosition(f, true);
    command::MatchesOracle(f, reference);
    Check(!original.Matches(f.first) &&
        originalNegative != NegativeTestRecord(*f.app, negative_frames::kPmmKey) && peer.Matches(f),
        "actual menu deletion changes both signs of A and leaves marked model B intact");
    const ModelKeyState forward(f.first);
    const target::Authors forwardAuthors(f);
    const auto forwardNegative = NegativeTestRecord(*f.app, negative_frames::kPmmKey);

    // One journal traverses B/B, camera/camera, then B/camera. Each destination
    // sees both directions; the last pair also switches context between them.
    // Negative, zero and positive replay frames do not replace the binding.
    const std::array<bool, 6> cameras{{false, false, true, true, false, true}};
    const std::array<int, 6> frames{{initialFrame < 0 ? 17 : -7, initialFrame < 0 ? 23 : -3,
        initialFrame < 0 ? -7 : 17, initialFrame < 0 ? -3 : 23, 0, 11}};
    for (unsigned step = 0; step < frames.size(); ++step) {
        const bool redo = step % 2 != 0;
        std::printf("    context %s camera=%u slot=1 frame=%d\n",
            redo ? "redo" : "undo", unsigned(cameras[step]), frames[step]);
        std::fflush(stdout);
        SelectContext(reference, cameras[step], frames[step]);
        SelectContext(f, cameras[step], frames[step]);
        const Selection selection(f);
        Check(peer.Matches(f), "switching context and seeking A leaves B unchanged before replay");
        Check(!plugins::BoundNativePoseHistory(reference.app.get()),
            "context replay oracle remains unbound before every directional preparation");
        oracle.Publish(redo, false);
        Check(plugins::BoundNativePoseHistory(f.app.get()) == &f.history &&
            plugins::BoundNativePoseGeneration(f.app.get()) == generation,
            "oracle and context switching preserve the uninterrupted actual journal binding");
        command::CheckCommandWindow(f);
        CmdControl400(f.app.get(), f.app->MainWindow(), redo ? 401 : 400, 0);
        // Fail if the actual command silently refuses replay; do not accept
        // MMS_E_STALE or route to a detached/private history as a substitute.
        command::HistoryPosition(f, redo);
        command::MatchesOracle(f, reference);
        Check((redo ? forward.Matches(f.first) : original.Matches(f.first)) &&
            (redo ? forwardAuthors.Matches(f) : originalAuthors.Matches(f)) &&
            NegativeTestRecord(*f.app, negative_frames::kPmmKey) == (redo ? forwardNegative : originalNegative),
            "context-independent undo/redo restores exact A native authored and negative directional data");
        Check(peer.Matches(f) && selection.Matches(f) && PoseRingDigest(f.first) == ring &&
            command::UnselectedNegativeSources(f) == unselected,
            "actual replay preserves B keys pose authoring marks current selection and both legacy rings");
        Check(plugins::BoundNativePoseHistory(f.app.get()) == &f.history &&
            plugins::BoundNativePoseGeneration(f.app.get()) == generation &&
            !reference.history.UndoCount() && !reference.history.RedoCount(),
            "all context replays use the same journal while the oracle has no history");
        command::NoPrivateHistory(f);
    }
}
} // namespace native_signed_delete_context_checks

void NativeSignedDeleteContextChecks() {
    using namespace native_signed_delete_context_checks;
    for (auto mode : {Mode::AllOperations, Mode::ExplicitOnly})
        for (bool automatic : {false, true})
            for (bool display : {false, true})
                for (int frame : {-5, 11}) ModelReplay(mode, automatic, display, frame);
}
