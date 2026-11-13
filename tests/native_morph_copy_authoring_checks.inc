// Include after the morph author fixtures/helpers in native_edit_history_tests.cpp.
// These checks exercise capture only; command 420 and paste belong to the caller.
#include <limits>

namespace {
struct MorphCopySourceSnapshot {
    MorphHostSnapshot author;
    std::uint64_t digest, ring;
    std::array<unsigned char, 2> selected;
    explicit MorphCopySourceSnapshot(MorphCurveWriteFixture& f)
        : author(f), digest(f.Digest()), ring(PoseRingDigest(f.first)), selected(f.first.selected) {}
    bool Matches(MorphCurveWriteFixture& f) const {
        std::uint64_t revision = 0;
        return f.Digest() == digest && PoseRingDigest(f.first) == ring &&
            f.first.selected == selected && author.Matches(f) &&
            f.history.Revision(revision) == MMS_OK && revision == author.revision &&
            f.history.UndoCount() == author.undo && f.history.RedoCount() == author.redo;
    }
};

animation_curve::NamedTrack MorphCopyInput(MorphCurveWriteFixture& f, unsigned morph) {
    auto result = f.Input(morph);
    for (std::size_t i = 0; i < result.track.keys.size(); ++i) {
        auto& key = result.track.keys[i];
        // Distinct doubles expose float-rounding and individual field loss.
        key.value += .012345678901234 + .03 * double(morph);
        key.inSlope = .031234567890123 + .01 * double(i);
        key.outSlope = -.027654321098765 - .02 * double(i);
        key.inWeight = .17 + .03 * double(i);
        key.outWeight = .41 - .04 * double(i);
    }
    return result;
}

animation_curve::Key MorphCopyNativeKey(int frame, float value) {
    animation_curve::Key key;
    key.frame = frame; key.value = double(value);
    key.interpolation = animation_curve::Interpolation::Linear;
    key.tangent = animation_curve::Tangent::Free;
    return key;
}

void MorphCopyCompleteOwnedCurves() {
    std::vector<morph_curve::CopyAuthoring> saved;
    animation_curve::Track expected0, expected1;
    {
        MorphCurveWriteFixture f;
        const auto first = MorphCopyInput(f, 0), second = MorphCopyInput(f, 1);
        expected0 = first.track; expected1 = second.track;
        std::string error;
        Check(morph_curve::Write(f.first.raw(), {first, second}, error), "seed copy author Bezier tracks");
        f.first.Entry(1, 1, 3); f.first.selected = {{1, 0}};
        for (unsigned morph : {0u, 1u}) {
            unsigned slot = morph;
            do {
                auto& key = f.first.m->morphKeys[slot];
                // Only frames 10 and 50 are selected: the unselected 25 key is
                // still the neighbor required to interpret both intervals.
                key.allocated = key.frame == 10 || key.frame == 50 ? 7 : 0;
                key.reserved[0] = 91; key.reserved[1] = 123;
                slot = key.next;
            } while (slot);
        }
        const MorphCopySourceSnapshot before(f);
        error = "old error";
        Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {1, 2, 1, 0, 0, 2}, saved, error) &&
            error.empty() && saved.size() == 2 && saved[0].morph == 1 && saved[1].morph == 0,
            "copy authors preserve first requested order, omit absent authors and deduplicate");
        Check(MorphHostSameTrack(saved[0].track, expected1) && MorphHostSameTrack(saved[1].track, expected0),
            "copy keeps every Bezier value slope weight mode and original absolute frame");
        Check(saved[1].track.keys.size() == 4 && saved[1].track.keys[2].frame == 25 &&
            saved[1].track.keys.front().frame == 0 && saved[1].track.keys.back().frame == 50,
            "disconnected source selection retains unselected adjacent author keys without rebasing");
        Check(before.Matches(f), "copy changes no author cache, native byte, pose, selection, cursor or undo state");
        auto editedOutput = saved;
        editedOutput[0].track.keys[0].outSlope += 7;
        std::vector<morph_curve::CopyAuthoring> recaptured;
        Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {1}, recaptured, error) &&
            recaptured.size() == 1 && MorphHostSameTrack(recaptured[0].track, expected1) && before.Matches(f),
            "editing an owned result cannot alter the live author or a second capture");
        auto replacement = first; replacement.track.keys[1].outSlope += .3;
        Check(morph_curve::Write(f.first.raw(), {replacement}, error), "replace author after retaining copy output");
        morph_curve::Forget(f.first.raw());
    }
    Check(saved.size() == 2 && MorphHostSameTrack(saved[0].track, expected1) &&
        MorphHostSameTrack(saved[1].track, expected0),
        "owned copy outlives author replacement, forgetting and destruction of the entire source");
}

void MorphCopyPrivateDerivationAndRestore() {
    MorphCurveWriteFixture f;
    const auto input = MorphCopyInput(f, 0);
    std::string error;
    Check(morph_curve::Write(f.first.raw(), {input}, error), "seed copy derivation author");
    const auto original = MorphKeysCopy(f.first);
    const MorphCopySourceSnapshot pristine(f);
    const unsigned changed = f.first.m->morphKeys[0].next;
    f.first.m->morphKeys[changed].value = .375f;
    auto expected = input.track;
    expected.keys[1] = MorphCopyNativeKey(10, .375f);
    expected.keys[2].inSlope = expected.keys[2].outSlope = 0;
    expected.keys[2].inWeight = expected.keys[2].outWeight = 1.0 / 3.0;
    expected.keys[2].interpolation = animation_curve::Interpolation::Linear;
    expected.keys[2].tangent = animation_curve::Tangent::Free;
    const MorphCopySourceSnapshot edited(f);
    std::vector<morph_curve::CopyAuthoring> out;
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {0}, out, error) && out.size() == 1 &&
        MorphHostSameTrack(out[0].track, expected) && edited.Matches(f),
        "capture privately derives current native edits without publishing effective or observed state");
    std::memcpy(f.first.m->morphKeys, original.data(), original.size() * sizeof(mdl::MorphKey));
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {0}, out, error) && out.size() == 1 &&
        MorphHostSameTrack(out[0].track, input.track) && pristine.Matches(f),
        "restoring the native pool recovers exact pristine controls after a read-only capture");

    morph_curve::AuthoredLayers retimed;
    auto retimeInput = input; retimeInput.track.keys[1].outWeight = .23;
    Check(morph_curve::RebuildLayers(f.first.raw(), {retimeInput}, retimed, error),
        "prepare valid author whose reference baseline is intentionally deferred");
    morph_curve::SwapLayers(f.first.raw(), retimed);
    const MorphCopySourceSnapshot deferred(f);
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {0}, out, error) && out.size() == 1 &&
        MorphHostSameTrack(out[0].track, retimeInput.track) && deferred.Matches(f),
        "deferred baseline is interpreted privately without installing mirror or observed entries");
    morph_curve::SwapLayers(f.first.raw(), retimed);
    Check(pristine.Matches(f), "copy leaves deferred layers exchange fully reversible");
}

void MorphCopyAbsenceIndicesAndIdentity() {
    // No author path must work with allocation denied, including before the
    // registry has ever been used when this entry point is run first.
    {
        Fixture f;
        const auto before = f.Digest();
        const std::vector<int> selected{0, 0};
        std::vector<morph_curve::CopyAuthoring> out(1);
        std::string error;
        bool ok = false, memory = false;
        failAfter = 0;
        try { ok = morph_curve::CaptureCopyAuthoring(f.raw(), selected, out, error); }
        catch (const std::bad_alloc&) { memory = true; }
        failAfter = -1;
        Check(ok && !memory && out.empty() && error.empty() && f.Digest() == before,
            "author-free copy succeeds empty without allocation or native changes");
        for (const std::vector<int>& invalid : {std::vector<int>{-1}, std::vector<int>{0, 1}}) {
            out.resize(1);
            Check(!morph_curve::CaptureCopyAuthoring(f.raw(), invalid, out, error) && out.empty() &&
                !error.empty() && f.Digest() == before, "indices are validated even without author state");
        }
        out.resize(1);
        Check(!morph_curve::CaptureCopyAuthoring(nullptr, selected, out, error) && out.empty() && !error.empty(),
            "null model rejects copy and clears previous output");
    }
    MorphCurveWriteFixture f;
    std::string error;
    const auto input = MorphCopyInput(f, 0);
    Check(morph_curve::Write(f.first.raw(), {input}, error), "seed selected absence and stale identity checks");
    const MorphCopySourceSnapshot before(f);
    std::vector<morph_curve::CopyAuthoring> out(1);
    const std::vector<int> absent{1, 2, 1};
    bool emptyOk = false, memory = false;
    failAfter = 0;
    try { emptyOk = morph_curve::CaptureCopyAuthoring(f.first.raw(), absent, out, error); }
    catch (const std::bad_alloc&) { memory = true; }
    failAfter = -1;
    Check(emptyOk && !memory && out.empty() && before.Matches(f),
        "ordinary native tracks never manufacture authoring or allocate copies of unrelated authors");
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {}, out, error) && out.empty() && before.Matches(f),
        "empty request leaves existing authors untouched");
    for (const std::vector<int>& invalid : {std::vector<int>{0, -1}, std::vector<int>{0, 3}}) {
        out.resize(1);
        Check(!morph_curve::CaptureCopyAuthoring(f.first.raw(), invalid, out, error) && out.empty() &&
            !error.empty() && before.Matches(f), "invalid indices reject the entire author request");
    }
    const auto nameByte = f.firstMorphs[0].name[0];
    f.firstMorphs[0].name[0] = '!';
    const auto stale = f.Digest();
    out.resize(1);
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {0}, out, error) && out.empty() && f.Digest() == stale,
        "identity mismatch has no author for this model and does not mutate the source");
    f.firstMorphs[0].name[0] = nameByte;
    Check(before.Matches(f) && morph_curve::CaptureCopyAuthoring(f.first.raw(), {0}, out, error) &&
        out.size() == 1 && MorphHostSameTrack(out[0].track, input.track) && before.Matches(f),
        "read-only stale lookup did not prune the author registry");
}

void MorphCopySelectedCorruption() {
    MorphCurveWriteFixture f;
    const auto first = MorphCopyInput(f, 0), second = MorphCopyInput(f, 1);
    std::string error;
    Check(morph_curve::Write(f.first.raw(), {first, second}, error), "seed independent copy author chains");
    const auto original = MorphKeysCopy(f.first);
    const MorphCopySourceSnapshot pristine(f);
    const unsigned continuation = f.first.m->morphKeys[1].next;
    const unsigned capacity = unsigned(mdl::MorphKeyCapacity(f.first.m->morphKeys));
    for (unsigned corruption = 0; corruption < 9; ++corruption) {
        std::memcpy(f.first.m->morphKeys, original.data(), original.size() * sizeof(mdl::MorphKey));
        auto& head = f.first.m->morphKeys[1];
        auto& key = f.first.m->morphKeys[continuation];
        switch (corruption) {
        case 0: key.next = capacity; break;
        case 1: key.next = continuation; break;
        case 2: key.previous = 2; break;
        case 3: key.frame = 0; break;
        case 4: head.frame = 1; break;
        case 5: key.value = (std::numeric_limits<float>::quiet_NaN)(); break;
        case 6: head.next = 2; break;
        case 7: head.previous = capacity; break;
        case 8: key.frame = unsigned(morph_curve::kMaximumFrame) + 1u; break;
        }
        const MorphCopySourceSnapshot damaged(f);
        std::vector<morph_curve::CopyAuthoring> out;
        Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {0}, out, error) && out.size() == 1 &&
            MorphHostSameTrack(out[0].track, first.track) && damaged.Matches(f),
            "unrequested damaged author chain does not affect capture or get pruned");
        Check(!morph_curve::CaptureCopyAuthoring(f.first.raw(), {0, 1}, out, error) && out.empty() &&
            !error.empty() && damaged.Matches(f),
            "requested malformed chain fails visibly, clears partial output and preserves all source state");
    }
    std::memcpy(f.first.m->morphKeys, original.data(), original.size() * sizeof(mdl::MorphKey));
    std::vector<morph_curve::CopyAuthoring> out;
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {1}, out, error) && out.size() == 1 &&
        MorphHostSameTrack(out[0].track, second.track) && pristine.Matches(f),
        "repaired native chain recovers its author after every rejected capture");
}

void MorphCopyAllocationFailures() {
    MorphCurveWriteFixture f;
    const auto first = MorphCopyInput(f, 0), second = MorphCopyInput(f, 1);
    std::string error;
    Check(morph_curve::Write(f.first.raw(), {first, second}, error), "seed copy allocation sweep authors");
    const auto original = MorphKeysCopy(f.first);
    const unsigned changed = f.first.m->morphKeys[0].next;
    const unsigned following = f.first.m->morphKeys[changed].next;
    unsigned inserted = f.first.m->morphCount;
    while (inserted < original.size() && f.first.m->morphKeys[inserted].frame) ++inserted;
    Check(inserted < original.size(), "copy allocation fixture has one private insertion slot");
    auto& key = f.first.m->morphKeys[inserted];
    key = {}; key.frame = 15; key.value = .625f; key.allocated = 1;
    key.previous = changed; key.next = following;
    f.first.m->morphKeys[changed].next = inserted;
    f.first.m->morphKeys[changed].value = .375f;
    f.first.m->morphKeys[following].previous = inserted;
    const MorphCopySourceSnapshot before(f);
    const std::vector<int> selected{0, 1, 0, 2, 1};
    std::vector<morph_curve::CopyAuthoring> expected;
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), selected, expected, error) && expected.size() == 2 &&
        expected[0].track.keys.size() == 5 && before.Matches(f), "prepare private insertion derivation oracle");
    unsigned failures = 0;
    bool completed = false;
    for (long limit = 0; limit < 4096; ++limit) {
        std::vector<morph_curve::CopyAuthoring> out = expected;
        bool ok = false, memory = false;
        failAfter = limit;
        try { ok = morph_curve::CaptureCopyAuthoring(f.first.raw(), selected, out, error); }
        catch (const std::bad_alloc&) { memory = true; }
        failAfter = -1;
        Check(before.Matches(f), "every allocation boundary preserves native, author, selection, pose and undo source");
        if (memory) {
            Check(!ok && out.empty(), "copy propagates bad_alloc to its caller with no stale or partial output");
            ++failures;
            continue;
        }
        Check(ok && error.empty() && out.size() == expected.size() &&
            MorphHostSameTrack(out[0].track, expected[0].track) && MorphHostSameTrack(out[1].track, expected[1].track),
            "first successful allocation sweep returns both complete privately derived authors");
        completed = true;
        break;
    }
    Check(completed && failures > 4, "copy allocation sweep reaches all allocation points and terminates");
    std::memcpy(f.first.m->morphKeys, original.data(), original.size() * sizeof(mdl::MorphKey));
    std::vector<morph_curve::CopyAuthoring> restored;
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {0, 1}, restored, error) && restored.size() == 2 &&
        MorphHostSameTrack(restored[0].track, first.track) && MorphHostSameTrack(restored[1].track, second.track),
        "allocation failures never overwrite pristine authoring needed by later native restore");
}

void MorphCopyInvalidEffectiveAuthor() {
    MorphCurveWriteFixture f;
    auto first = MorphCopyInput(f, 0);
    const auto second = MorphCopyInput(f, 1);
    // This is valid while the frame-10 key exists. Its removal gives the
    // following key native handles (1/3), incompatible with the retained .8
    // outgoing handle. A plausible linear fallback must not hide that error.
    first.track.keys[0].outWeight = .8;
    first.track.keys[1].inWeight = .1;
    std::string error;
    Check(morph_curve::Write(f.first.raw(), {first, second}, error), "seed valid wide-handle copy authors");
    const auto original = MorphKeysCopy(f.first);
    const MorphCopySourceSnapshot pristine(f);
    const unsigned removed = f.first.m->morphKeys[0].next;
    const unsigned following = f.first.m->morphKeys[removed].next;
    f.first.m->morphKeys[0].next = following;
    f.first.m->morphKeys[following].previous = 0;
    f.first.m->morphKeys[removed] = {};
    const MorphCopySourceSnapshot edited(f);
    std::vector<morph_curve::CopyAuthoring> out(1);
    Check(!morph_curve::CaptureCopyAuthoring(f.first.raw(), {1, 0}, out, error) && out.empty() &&
        !error.empty() && edited.Matches(f),
        "invalid privately derived author rejects the whole copy without silently dropping its author");
    std::memcpy(f.first.m->morphKeys, original.data(), original.size() * sizeof(mdl::MorphKey));
    Check(morph_curve::CaptureCopyAuthoring(f.first.raw(), {0}, out, error) && out.size() == 1 &&
        MorphHostSameTrack(out[0].track, first.track) && pristine.Matches(f),
        "failed effective-author validation preserves the original wide controls for native restore");
}
} // namespace

void NativeMorphCopyAuthoringChecks() {
    MorphCopyAbsenceIndicesAndIdentity();
    MorphCopyCompleteOwnedCurves();
    MorphCopyPrivateDerivationAndRestore();
    MorphCopySelectedCorruption();
    MorphCopyInvalidEffectiveAuthor();
    MorphCopyAllocationFailures();
}
