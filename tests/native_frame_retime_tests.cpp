// MAINTENANCE: standalone CPU coverage for include/mikumikustudio/
// native_frame_retime.hpp and src/animation/native_frame_retime.cpp.
//
// The suite builds a synthetic project by hand: two models (one of them not the
// selected one), a morph and a display chain per model, all four application
// tables and two accessories.  It then pins the parts of the contract that are
// easy to get wrong:
//
//   * Prepare changes nothing, not one byte of any pool;
//   * the counters are per native key RECORD, so a bone record with four
//     interpolation channels and a quaternion is one key, not four or eight;
//   * the change list names the exact frames: one Rounded entry for every key
//     whose own frame moved, one Merged entry for every key from the second of
//     a collision group on, and both for a key that is both;
//   * an edit between Prepare and Commit invalidates the plan and leaves the
//     project alone, and putting the byte back makes the plan valid again;
//   * a finalize callback that refuses rolls the whole conversion back byte for
//     byte, scene flag included;
//   * a successful commit retimes every model including the unselected one, and
//     a plan cannot be committed twice.
//
// The three side-table modules (bone_scale, morph_curve, curve_host) are not
// linked here; the stubs at the bottom stand in for them so this file stays a
// pure CPU test.  Build with -DMDS_NATIVE_FRAME_RETIME_STANDALONE to bring the
// stubs in; the unified build defines nothing and links the real modules.
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/frame_rate_correction.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/native_frame_retime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mikumikustudio/animation_curve.hpp"

namespace {

int g_failures = 0;

void Check(bool pass, const char* text) {
    if (pass) return;
    ++g_failures;
    std::printf("FAIL %s\n", text);
}

void CheckEqual(std::uint64_t actual, std::uint64_t expected, const char* text) {
    if (actual == expected) return;
    ++g_failures;
    std::printf("FAIL %s: expected %llu, got %llu\n", text, static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(actual));
}

std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// One digest over every pool of the project, so "changed nothing" is a single
// comparison and it covers bytes no assertion names.
struct Fixture {
    std::unique_ptr<mikumikustudio::MMDApp> app;
    std::unique_ptr<unsigned char[]> object[2];
    std::vector<mikumikustudio::mdl::BoneKey> boneKeys[2];
    std::vector<mikumikustudio::mdl::MorphKey> morphKeys[2];
    std::vector<mikumikustudio::mdl::DisplayKey> displayKeys[2];
    std::vector<mikumikustudio::mdl::CameraKey> camera;    std::vector<mikumikustudio::mdl::LightKey> light;
    std::vector<mikumikustudio::mdl::SelfShadowKey> shadow;
    std::vector<mikumikustudio::mdl::GravityKey> gravity;
    std::vector<mikumikustudio::mdl::AccessoryKey> accessory[2];
    mikumikustudio::mdl::AccessoryRecord accessoryRecord[2]{};
    // Display keys own two heap arrays each.  Four records per model, so the
    // store is sliced: record r takes ikStore[slot] + r*ikChainCount.
    std::vector<std::uint8_t> ikStore[2];
    std::vector<mikumikustudio::mdl::BoneReference> selectorStore[2];
    // Real record tables, not null pointers: the morph and bone tables are what
    // the morph-curve and bone-scale modules read, so the fixture has to carry
    // them for a build that links the real modules.
    std::vector<mikumikustudio::mdl::MorphRecord> morphTable[2];
    std::vector<mikumikustudio::mdl::BoneRecord> boneTable[2];

    unsigned char* Model(int slot) { return object[slot].get(); }

    // Every display record owns two heap arrays and every model owns entries in
    // the bone scale and morph curve side tables.  Both are keyed by the model
    // address, so a fixture that leaves them behind would let the next fixture's
    // recycled address inherit them.  A commit that dropped a record has already
    // freed that record's arrays and nulled the pointers, so freeing through the
    // record is safe for every case in this file.
    ~Fixture() {
#ifndef MDS_NATIVE_FRAME_RETIME_STANDALONE
        if (app) mikumikustudio::curve_host::Reset(app.get());
#endif
        for (int slot = 0; slot < 2; ++slot) {
            for (auto& key : displayKeys[slot]) {
                std::free(key.ikStates);
                std::free(key.selectorStates);
                key.ikStates = nullptr;
                key.selectorStates = nullptr;
            }
#ifndef MDS_NATIVE_FRAME_RETIME_STANDALONE
            if (object[slot]) {
                mikumikustudio::bone_scale::Forget(object[slot].get());
                mikumikustudio::morph_curve::Forget(object[slot].get());
            }
#endif
        }
    }

    std::uint64_t Digest() const {
        std::uint64_t hash = 1469598103934665603ull;
        for (int slot = 0; slot < 2; ++slot) {
            hash = HashBytes(hash, object[slot].get(), sizeof(mikumikustudio::mdl::ModelRecord));
            hash = HashBytes(hash, boneKeys[slot].data(),
                             boneKeys[slot].size() * sizeof(mikumikustudio::mdl::BoneKey));
            hash = HashBytes(hash, morphKeys[slot].data(),
                             morphKeys[slot].size() * sizeof(mikumikustudio::mdl::MorphKey));
            hash = HashBytes(hash, displayKeys[slot].data(),
                             displayKeys[slot].size() * sizeof(mikumikustudio::mdl::DisplayKey));
            hash = HashBytes(hash, accessory[slot].data(),
                             accessory[slot].size() * sizeof(mikumikustudio::mdl::AccessoryKey));
        }
        hash = HashBytes(hash, camera.data(), camera.size() * sizeof(mikumikustudio::mdl::CameraKey));
        hash = HashBytes(hash, light.data(), light.size() * sizeof(mikumikustudio::mdl::LightKey));
        hash = HashBytes(hash, shadow.data(), shadow.size() * sizeof(mikumikustudio::mdl::SelfShadowKey));
        hash = HashBytes(hash, gravity.data(), gravity.size() * sizeof(mikumikustudio::mdl::GravityKey));
        return hash;
    }
};

using mikumikustudio::mdl::BoneKey;
using mikumikustudio::mdl::DisplayKey;
using mikumikustudio::mdl::MorphKey;

// A three-key bone chain (frames 0, 1, 2) on bone 3, a two-key morph chain on
// morph 1, a two-key display chain, and a two-key chain in every global and
// accessory table.  Ratio 0.5 turns the bone chain's frames 1 and 2 into one
// collision on frame 1, which is the case the change list exists for.
void Build(Fixture& fixture) {
    fixture.app = std::make_unique<mikumikustudio::MMDApp>();
    // MMDApp's constructor is a no-op and the production Init zeroes the state
    // explicitly, so a fixture has to do the same.  Left unzeroed, every
    // accessory slot this fixture does not fill holds the debug allocator's
    // 0xBAADF00D, and the first real name read walks those slots and crashes
    // inside text_encoding::Display.
    fixture.app->state = mikumikustudio::MMDAppState{};
    auto& app = *fixture.app;
    for (int slot = 0; slot < 2; ++slot) {
        fixture.object[slot].reset(new unsigned char[sizeof(mikumikustudio::mdl::ModelRecord)]());
        unsigned char* model = fixture.object[slot].get();
        fixture.boneKeys[slot].assign(600000, BoneKey{});
        fixture.morphKeys[slot].assign(20000, MorphKey{});
        fixture.displayKeys[slot].assign(1000, DisplayKey{});
        mikumikustudio::mdl::BoneKeys(model) = fixture.boneKeys[slot].data();
        mikumikustudio::mdl::MorphKeys(model) = fixture.morphKeys[slot].data();
        mikumikustudio::mdl::DisplayKeys(model) = fixture.displayKeys[slot].data();
        auto& record = *mikumikustudio::mdl::Mdl(model);
        record.boneCount = 4;
        record.morphCount = 2;
        // The morph and bone tables the real modules read.  Morph 1 gets a name
        // so a stable id can be built from it, which is what the expression
        // module keys its authoring on.
        fixture.morphTable[slot].assign(2, mikumikustudio::mdl::MorphRecord{});
        std::memcpy(fixture.morphTable[slot][1].name, "eye1", 4);
        fixture.boneTable[slot].assign(4, mikumikustudio::mdl::BoneRecord{});
        std::memcpy(fixture.boneTable[slot][3].name, "center", 6);
        record.morphs = fixture.morphTable[slot].data();
        record.boneTable = fixture.boneTable[slot].data();
        record.ikChainCount = 2;
        record.boneOrderCount = 1;
        record.maxFrame = 3;

        BoneKey* bones = fixture.boneKeys[slot].data();
        bones[3].frame = 1;
        bones[3].next = 4;
        bones[3].allocated = 1;
        bones[4].frame = 2;
        bones[4].previous = 3;
        bones[4].next = 5;
        bones[4].allocated = 1;
        bones[5].frame = 3;
        bones[5].previous = 4;
        bones[5].allocated = 1;
        bones[4].position[0] = 1.0f;
        bones[5].position[0] = 2.0f;

        MorphKey* morphs = fixture.morphKeys[slot].data();
        morphs[1].frame = 0;
        morphs[1].next = 2;
        morphs[1].allocated = 1;
        morphs[2].frame = 30;
        morphs[2].previous = 1;
        morphs[2].allocated = 1;

        // The display chain is frames 0, 2, 3.  At 30 -> 12 they land on 0, 1 and
        // 1, so the group {2,3} collides and its survivor must end up holding the
        // frame-3 state, while the frame-0 record keeps its own.  Every record
        // owns its two arrays, and the states are deliberately different.
        DisplayKey* display = fixture.displayKeys[slot].data();
        const int frames[3] = {0, 2, 3};
        for (int record = 0; record < 3; ++record) {
            const std::size_t slotIndex = record == 1 ? 2 : record == 2 ? 3 : 0;
            display[slotIndex].frame = frames[record];
            display[slotIndex].previous = record == 0 ? 0 : (record == 1 ? 0u : 2u);
            display[slotIndex].allocated = 1;
            display[slotIndex].visible = record == 0 ? 1 : 0;
            // Each record owns its two arrays, allocated the way the host
            // allocates them: the conversion frees the arrays of a record it
            // drops, so they must not point into shared storage.
            display[slotIndex].ikStates = static_cast<std::uint8_t*>(std::malloc(2));
            display[slotIndex].selectorStates = static_cast<mikumikustudio::mdl::BoneReference*>(
                std::malloc(sizeof(mikumikustudio::mdl::BoneReference)));
            display[slotIndex].ikStates[0] = static_cast<std::uint8_t>(record + 1);
            display[slotIndex].ikStates[1] = static_cast<std::uint8_t>(9 - record);
            display[slotIndex].selectorStates[0].modelIndex = record + 1;
            display[slotIndex].selectorStates[0].boneIndex = record + 5;
            fixture.ikStore[slot].push_back(display[slotIndex].ikStates[0]);
            fixture.ikStore[slot].push_back(display[slotIndex].ikStates[1]);
            fixture.selectorStore[slot].push_back(display[slotIndex].selectorStates[0]);
        }
        display[0].next = 2;
        display[2].next = 3;
        display[3].next = 0;

        app.ModelSlot(slot) = model;
    }
    app.SelectedModelSlot() = 0;
    app.EnglishUI() = 1;
    app.SceneModified() = 0;

    fixture.camera.assign(10000, mikumikustudio::mdl::CameraKey{});
    fixture.light.assign(10000, mikumikustudio::mdl::LightKey{});
    fixture.shadow.assign(10000, mikumikustudio::mdl::SelfShadowKey{});
    fixture.gravity.assign(10000, mikumikustudio::mdl::GravityKey{});
    fixture.camera[0].frame = 0;
    fixture.camera[0].next = 1;
    fixture.camera[1].frame = 30;
    fixture.camera[1].previous = 0;
    fixture.light[0].frame = 0;
    fixture.light[0].next = 1;
    fixture.light[1].frame = 30;
    fixture.light[1].previous = 0;
    fixture.shadow[0].frame = 0;
    fixture.shadow[0].next = 1;
    fixture.shadow[1].frame = 30;
    fixture.shadow[1].previous = 0;
    fixture.gravity[0].frame = 0;
    fixture.gravity[0].next = 1;
    fixture.gravity[1].frame = 30;
    fixture.gravity[1].previous = 0;
    app.CameraKeys() = fixture.camera.data();
    app.LightKeys() = fixture.light.data();
    app.ShadowKeys() = fixture.shadow.data();
    app.GravityKeys() = fixture.gravity.data();

    for (int slot = 0; slot < 2; ++slot) {
        fixture.accessory[slot].assign(10000, mikumikustudio::mdl::AccessoryKey{});
        fixture.accessory[slot][0].frame = 0;
        fixture.accessory[slot][0].next = 1;
        fixture.accessory[slot][0].scale = 1.0f;
        fixture.accessory[slot][1].frame = 30;
        fixture.accessory[slot][1].previous = 0;
        fixture.accessory[slot][1].scale = 1.0f;
        app.AccessorySlot(slot) = &fixture.accessoryRecord[slot];
        app.AccessoryKeys(slot) = fixture.accessory[slot].data();
    }
}

const mikumikustudio::frame_rate_correction::FrameChange* FindChange(
    const mikumikustudio::frame_rate_correction::Report& report,
    mikumikustudio::frame_rate_correction::FrameChange::ChangeKind kind, std::int32_t originalFrame) {
    for (const auto& change : report.changes) {
        if (change.kind == kind && change.originalFrame == originalFrame) return &change;
    }
    return nullptr;
}

// The frozen rule, checked on the whole list rather than on one example:
// every Rounded entry carries a rounding offset (exactFrame - newFrame is
// larger than the tolerance) and newFrame is round(exactFrame); every entry is
// one cause; and the list accounts for every counter without truncation.
void CheckChangeList(const mikumikustudio::frame_rate_correction::Report& report, const char* what) {
    using mikumikustudio::frame_rate_correction::FrameChange;
    std::uint64_t rounded = 0;
    std::uint64_t merged = 0;
    for (const auto& change : report.changes) {
        if (change.kind == FrameChange::ChangeKind::Merged) {
            ++merged;
            continue;
        }
        ++rounded;
        const double offset = change.exactFrame - static_cast<double>(change.newFrame);
        if (!(std::fabs(offset) > 1e-12)) {
            ++g_failures;
            std::printf("FAIL %s: a Rounded entry carries no rounding offset (frame %d)\n", what,
                        change.originalFrame);
        }
        if (change.newFrame != static_cast<std::int32_t>(std::llround(change.exactFrame))) {
            ++g_failures;
            std::printf("FAIL %s: a Rounded entry's newFrame is not round(exactFrame) (frame %d)\n",
                        what, change.originalFrame);
        }
    }
    CheckEqual(rounded, report.roundedKeys, "the Rounded entries match roundedKeys");
    CheckEqual(merged, report.mergedKeys, "the Merged entries match mergedKeys");
    CheckEqual(report.changes.size(), report.roundedKeys + report.mergedKeys,
               "every detail entry is one cause");
    Check(!report.detailsTruncated, "the detail list is never truncated");
    CheckEqual(report.omittedDetailCount, 0, "no detail entry is ever omitted");
}

// Native curve shapes the fixture plants into the real records.  A legal
// record may hold any of them and a conversion must not refuse the project
// because of one: an ordinary non-linear ease, handles that cross (x1 >= x2),
// and handles that touch the ends of the 0..127 domain.  The expected result
// never comes from these constants - it comes from the bytes the record held
// before and from the model's own curve sampled before and after.
#ifndef MDS_NATIVE_FRAME_RETIME_STANDALONE
const std::uint8_t kEase[4] = {20, 40, 107, 120};
const std::uint8_t kCrossing[4] = {100, 20, 50, 30};
const std::uint8_t kEdge[4] = {0, 30, 127, 100};

namespace ac = mikumikustudio::animation_curve;
namespace bs = mikumikustudio::bone_scale;
namespace mc = mikumikustudio::morph_curve;
namespace ch = mikumikustudio::curve_host;

// A bone key stores its sixteen interpolation bytes as four control rows of
// four channels; a camera key stores four control rows of six channels.  These
// two write one channel's tuple the way the record lays it out.
void SetBoneChannel(mikumikustudio::mdl::BoneKey& key, int channel, const std::uint8_t* tuple) {
    for (int control = 0; control < 4; ++control)
        key.interpolation[control * 4 + channel] = tuple[control];
}

void SetCameraChannel(mikumikustudio::mdl::CameraKey& key, int channel, const std::uint8_t* tuple) {
    for (int control = 0; control < 4; ++control)
        key.interpolation[control][channel] = tuple[control];
}

// Gives the camera chain a third key so that a 30 -> 12 conversion collides
// there (frames 0, 2, 3 land on 0, 1 and 1), while 30 -> 45 keeps the three
// apart (0, 3, 5).  Both cases then share one fixture shape.
void GiveCameraThreeKeys(Fixture& fixture) {
    fixture.camera[1].frame = 2;
    fixture.camera[1].previous = 0;
    fixture.camera[1].next = 2;
    fixture.camera[2].frame = 3;
    fixture.camera[2].previous = 1;
    fixture.camera[2].next = 0;
}

// Fills bone 3's chain and the camera chain with authored curves, including one
// crossing tuple and one end-handled tuple, and leaves one interval of each
// chain with equal endpoint values behind a non-linear ease.
void PlantNativeShapes(Fixture& fixture) {
    for (int slot = 0; slot < 2; ++slot) {
        mikumikustudio::mdl::BoneKey* bones = fixture.boneKeys[slot].data();
        for (int key = 3; key <= 5; ++key)
            for (int channel = 0; channel < 4; ++channel)
                SetBoneChannel(bones[key], channel, kEase);
        // The chain head carries a tuple as well: a rewrite that clears the
        // first key's controls shows up here.
        SetBoneChannel(bones[3], 0, kCrossing);
        SetBoneChannel(bones[4], 1, kCrossing);
        SetBoneChannel(bones[5], 2, kEdge);
        // Equal endpoint values behind a non-linear ease on the first interval.
        bones[3].position[0] = 1.0f;
        bones[4].position[0] = 1.0f;
    }
    mikumikustudio::mdl::CameraKey* camera = fixture.camera.data();
    for (int key = 0; key < 3; ++key)
        for (int channel = 0; channel < 6; ++channel)
            SetCameraChannel(camera[key], channel, kEase);
    SetCameraChannel(camera[0], 3, kCrossing);
    SetCameraChannel(camera[1], 4, kEdge);
    // Equal endpoint values on the rotation lane, whose ease is not linear.
    camera[0].target[0] = 2.0f;
    camera[1].target[0] = 2.0f;
}

void CheckBytes(const void* actual, const void* expected, std::size_t size, const char* text) {
    if (std::memcmp(actual, expected, size) == 0) return;
    ++g_failures;
    const auto* got = static_cast<const std::uint8_t*>(actual);
    const auto* want = static_cast<const std::uint8_t*>(expected);
    for (std::size_t index = 0; index < size; ++index) {
        if (got[index] != want[index]) {
            std::printf("FAIL %s: byte %d is %d, expected %d\n", text, static_cast<int>(index),
                        static_cast<int>(got[index]), static_cast<int>(want[index]));
            return;
        }
    }
}
#endif  // MDS_NATIVE_FRAME_RETIME_STANDALONE

}  // namespace

int main() {
    using mikumikustudio::frame_rate_correction::FrameChange;
    using mikumikustudio::frame_rate_correction::Rates;
    using mikumikustudio::frame_rate_correction::Report;
    using mikumikustudio::native_frame_retime::Commit;
    using mikumikustudio::native_frame_retime::PlanPtr;
    using mikumikustudio::native_frame_retime::Prepare;

    // ---- a fractional rate first (29.97 -> 30): every key that lands off a
    //      frame must be reported, with the offset that makes it a rounding ----
    {
        Rates fractional;
        fractional.originalFPS = 29.97;
        fractional.currentFPS = 30.0;
        Fixture other;
        Build(other);
        const std::uint64_t untouched = other.Digest();
        PlanPtr otherPlan;
        Report otherReport;
        std::wstring fractionError;
        Check(Prepare(*other.app, fractional, otherPlan, otherReport, fractionError),
              "the fractional rate prepares");
        CheckEqual(other.Digest(), untouched, "the fractional Prepare is read-only");
        // Bone frames 1..3 land on 1.001, 2.002 and 3.003, display frames 2 and
        // 3 on 2.002 and 3.003, and every frame-30 key of the other chains on
        // 30.03, so all of them need rounding and none of them collides.  Each
        // model contributes twelve keys: bone 3's three, one key for each of
        // bones 0, 1 and 2, morph 1's two, one for morph 0, and the three
        // display keys.  The four keys per model that occupy a chain root with
        // no motion - bones 0, 1 and 2 and morph 0 - are reachable, so they are
        // examined and counted like any other key even though they carry no
        // export mark; across the two models those eight keys are exactly what
        // separates this count from the one that only counted authored keys.
        // The remaining twelve are the eight global keys and the four accessory
        // keys.  2 x 12 + 8 + 4 = 36, and with no collision the output count is
        // the same.
        CheckEqual(otherReport.inputKeys, 36, "every reachable key, including unselected zero baselines, is counted once");
        CheckEqual(otherReport.outputKeys, 36, "the fractional case merges nothing away");
        CheckEqual(otherReport.mergeGroups, 0, "the fractional case has no collision group");
        CheckEqual(otherReport.mergedKeys, 0, "the fractional case merges no key");
        CheckEqual(otherReport.roundedKeys, 18, "every off-frame key counts as a rounding");
        CheckEqual(otherReport.changes.size(), 18, "the fractional list is complete");
        CheckChangeList(otherReport, "29.97 -> 30");
        const FrameChange* entry = FindChange(otherReport, FrameChange::ChangeKind::Rounded, 3);
        if (entry != nullptr) {
            Check(entry->newFrame == 3, "the fractional entry reports its rounded frame");
            const double expected = 3.0 * 30.0 / 29.97 - 3.0;
            Check(std::fabs((entry->exactFrame - 3.0) - expected) < 1e-9,
                  "the fractional entry reports its rounding offset");
        } else {
            Check(false, "the fractional case lists the key at frame 3");
        }
        Check(FindChange(otherReport, FrameChange::ChangeKind::Merged, 30) == nullptr,
              "a key that keeps its slot is not merged at a fractional rate");
    }

    Fixture fixture;
    Build(fixture);
    const std::uint64_t before = fixture.Digest();
    const std::vector<std::uint8_t> ikBefore = fixture.ikStore[0];
    const std::vector<mikumikustudio::mdl::BoneReference> selectorBefore = fixture.selectorStore[0];

    // Ratio 30 -> 12, that is 2/5.  The bone chain's frames 1, 2 and 3 land on
    // 0.4, 0.8 and 1.2: all three need rounding, and 2 and 3 share frame 1.  The
    // frame-30 key of every other chain lands on 12 exactly, which is an
    // ordinary move and must not be reported as a rounding at all.
    Rates rates;
    rates.originalFPS = 30.0;
    rates.currentFPS = 12.0;

    PlanPtr plan;
    Report report;
    std::wstring error;
    Check(Prepare(*fixture.app, rates, plan, report, error), "Prepare succeeds");
    Check(plan != nullptr, "Prepare fills the plan");
    CheckEqual(fixture.Digest(), before, "Prepare is read-only");
    // The display payloads live behind pointers in the records, so they need
    // their own check: a Prepare that wrote through them would show up here.
    // The display payloads live behind pointers in the records, so they need
    // their own check: a Prepare that wrote through them would show up here.
    bool payloadsUnchanged = true;
    for (int record = 0; record < 3; ++record) {
        const std::size_t slotIndex = record == 1 ? 2 : record == 2 ? 3 : 0;
        if (fixture.displayKeys[0][slotIndex].ikStates[0] != fixture.ikStore[0][record * 2] ||
            fixture.displayKeys[0][slotIndex].ikStates[1] != fixture.ikStore[0][record * 2 + 1]) {
            payloadsUnchanged = false;
        }
        if (std::memcmp(&fixture.displayKeys[0][slotIndex].selectorStates[0],
                        &fixture.selectorStore[0][record],
                        sizeof(mikumikustudio::mdl::BoneReference)) != 0) {
            payloadsUnchanged = false;
        }
    }
    Check(payloadsUnchanged, "Prepare never writes a display payload");

    CheckEqual(report.models, 2, "both models are examined");
    CheckEqual(report.accessories, 2, "both accessories are examined");
    CheckEqual(report.mmvTracks, 0, "MMV tracks belong to another module");
    CheckEqual(report.meshCaches, 0, "mesh caches belong to another module");
    // Per record: 2 models x (3 bone + 2 morph + 3 display) + 4 globals x 2 + 2 accessories x 2.
    CheckEqual(report.inputKeys, 36, "every reachable key, including unselected zero baselines, is counted once");
    CheckEqual(report.outputKeys, 32, "the merged keys leave the output");
    CheckEqual(report.mergeGroups, 4, "one collision group per model per merged track");
    // Rounded counts the keys whose exact frame was not a frame: the three bone
    // keys and display frames 2 and 3 of each model, and nothing else, because
    // the frame-30 keys all land on 12 exactly.  One entry per RECORD, so the
    // four interpolation channels and the quaternion of a bone key contribute
    // one, not four.
    CheckEqual(report.roundedKeys, 10, "rounded keys are counted per record, not per component");
    CheckEqual(report.mergedKeys, 4, "one merged key per collision group");
    CheckEqual(report.changes.size(), 14, "the detail list holds one entry per cause");
    Check(!report.detailsTruncated, "the detail list is complete");
    CheckEqual(report.omittedDetailCount, 0, "nothing is left out of the detail list");

    // The key that both needed rounding and was merged is listed once for each
    // cause, in each model.  Its exact frame is 3 * 2/5 = 1.2.
    const FrameChange* rounded = FindChange(report, FrameChange::ChangeKind::Rounded, 3);
    Check(rounded != nullptr, "the rounded key is listed");
    if (rounded != nullptr) {
        Check(rounded->newFrame == 1, "the rounded key reports its new frame");
        Check(std::fabs(rounded->exactFrame - 1.2) < 1e-9, "the rounded key reports its exact frame");
    }
    const FrameChange* merged = FindChange(report, FrameChange::ChangeKind::Merged, 3);
    Check(merged != nullptr, "the merged-away key is listed");
    if (merged != nullptr) {
        Check(merged->newFrame == 1, "the merged key reports the shared frame");
        Check(merged->mergeFirstFrame == 2 && merged->mergeLastFrame == 3,
              "the merged key reports the group's original frame range");
        Check(std::fabs(merged->exactFrame - 1.2) < 1e-9, "the merged key reports its exact frame");
    }
    int causesForMergedKey = 0;
    for (const auto& change : report.changes) {
        if (change.originalFrame == 3) ++causesForMergedKey;
    }
    CheckEqual(static_cast<std::uint64_t>(causesForMergedKey), 8,
               "each model's bone and display key at frame 3 reports both causes");
    // A proportional move that lands exactly on a frame is a normal move.
    Check(FindChange(report, FrameChange::ChangeKind::Rounded, 30) == nullptr,
          "a move that lands on an exact frame is not reported as a rounding");
    Check(FindChange(report, FrameChange::ChangeKind::Merged, 30) == nullptr,
          "a key that keeps its slot is not reported as merged");
    CheckChangeList(report, "30 -> 12");

    // An edit between Prepare and Commit invalidates the plan.
    fixture.boneKeys[0][5].position[0] = 9.0f;
    const std::uint64_t edited = fixture.Digest();
    Check(!Commit(*fixture.app, plan, error, {}), "an edit invalidates the plan");
    CheckEqual(fixture.Digest(), edited, "the invalidated commit changed nothing");
    fixture.boneKeys[0][5].position[0] = 2.0f;
    CheckEqual(fixture.Digest(), before, "putting the byte back restores the plan's validity");

    // A key registered into a slot that was free at Prepare invalidates the plan
    // as well: the pool changed even though no previously occupied slot did.
    fixture.boneKeys[0][0].frame = 7;
    fixture.boneKeys[0][0].allocated = 1;
    const std::uint64_t registered = fixture.Digest();
    Check(!Commit(*fixture.app, plan, error, {}), "a key in a formerly free slot invalidates the plan");
    CheckEqual(fixture.Digest(), registered, "the invalidated commit changed nothing");
    fixture.boneKeys[0][0] = BoneKey{};
    CheckEqual(fixture.Digest(), before, "clearing the new key restores the plan's validity");

    // A refusing finalize callback rolls the whole conversion back.
    Check(!Commit(*fixture.app, plan, error, [](std::wstring&) { return false; }),
          "a refusing callback cancels the commit");
    CheckEqual(fixture.Digest(), before, "the cancelled commit rolled back byte for byte");
    CheckEqual(fixture.app->SceneModified(), 0, "the cancelled commit left the scene flag alone");

    // The real commit.  It is also the retry after the rejected callback, so a
    // success here proves the failed attempt left nothing behind for the second
    // one to undo again.
    Check(Commit(*fixture.app, plan, error, {}), "the plan is retryable and the retry succeeds");
    CheckEqual(static_cast<std::uint64_t>(fixture.boneKeys[0][3].frame), 0, "the first group's survivor takes its frame");
    CheckEqual(static_cast<std::uint64_t>(fixture.boneKeys[0][4].frame), 1, "the collision group's survivor takes the shared frame");
    CheckEqual(static_cast<std::uint64_t>(fixture.boneKeys[0][5].frame), 0, "the merged-away slot left the chain");
    CheckEqual(static_cast<std::uint64_t>(fixture.boneKeys[0][3].next), 4, "the chain is reconnected");
    CheckEqual(static_cast<std::uint64_t>(fixture.boneKeys[1][4].frame), 1,
               "the model that is not selected is retimed too");
    CheckEqual(static_cast<std::uint64_t>(fixture.morphKeys[0][2].frame), 12, "the morph chain is retimed");
    CheckEqual(static_cast<std::uint64_t>(fixture.displayKeys[0][2].frame), 1, "the display chain is retimed");
    // The display payloads: frame 3's state moved into the surviving record 2,
    // and record 0 kept its own.
    CheckEqual(static_cast<std::uint64_t>(fixture.displayKeys[0][2].ikStates[0]), 3,
               "the merged record took the group's last IK state");
    CheckEqual(static_cast<std::uint64_t>(fixture.displayKeys[0][2].ikStates[1]), 7,
               "the merged record took the whole last IK array");
    CheckEqual(static_cast<std::uint64_t>(fixture.displayKeys[0][2].selectorStates[0].modelIndex), 3,
               "the merged record took the group's last selector state");
    CheckEqual(static_cast<std::uint64_t>(fixture.displayKeys[0][2].selectorStates[0].boneIndex), 7,
               "the merged record took the whole last selector state");
    CheckEqual(static_cast<std::uint64_t>(fixture.displayKeys[0][0].ikStates[0]), 1,
               "the frame-zero record keeps its own IK state");
    CheckEqual(static_cast<std::uint64_t>(fixture.displayKeys[0][0].selectorStates[0].modelIndex), 1,
               "the frame-zero record keeps its own selector state");
    CheckEqual(static_cast<std::uint64_t>(fixture.camera[1].frame), 12, "the camera track is retimed");
    CheckEqual(static_cast<std::uint64_t>(fixture.light[1].frame), 12, "the light track is retimed");
    CheckEqual(static_cast<std::uint64_t>(fixture.shadow[1].frame), 12, "the shadow track is retimed");
    CheckEqual(static_cast<std::uint64_t>(fixture.gravity[1].frame), 12, "the gravity track is retimed");
    CheckEqual(static_cast<std::uint64_t>(fixture.accessory[0][1].frame), 12, "an accessory track is retimed");
    CheckEqual(static_cast<std::uint64_t>(fixture.accessory[1][1].frame), 12, "the second accessory is retimed");
    CheckEqual(fixture.app->SceneModified(), 1, "a successful commit marks the scene");
    CheckEqual(static_cast<std::uint64_t>(mikumikustudio::mdl::Mdl(fixture.Model(0))->maxFrame), 12,
               "the model timeline bound is recomputed");
    Check(!Commit(*fixture.app, plan, error, {}), "a plan is committed at most once");

    // =====================================================================
    // Real-core regressions.  These drive the real bone_scale, morph_curve and
    // curve_host modules the CMake target links; the standalone build stubs
    // them and so is kept out entirely.
    // =====================================================================
#ifndef MDS_NATIVE_FRAME_RETIME_STANDALONE

    // ---- no collision: every interpolation byte survives ------------------
    // 30 -> 45 leaves every chain's keys apart, so the conversion only moves
    // keys.  The records carry a chain head whose controls are non-linear, an
    // interval with equal endpoint values behind a non-linear ease, a crossing
    // tuple and an end-handled tuple.  None of the sixteen bone bytes and none
    // of the twenty-four camera bytes may change.
    {
        Fixture plain;
        Build(plain);
        GiveCameraThreeKeys(plain);
        PlantNativeShapes(plain);

        std::uint8_t boneBefore[2][3][16];
        std::uint8_t cameraBefore[3][24];
        for (int slot = 0; slot < 2; ++slot)
            for (int key = 3; key <= 5; ++key)
                std::memcpy(boneBefore[slot][key - 3], plain.boneKeys[slot][key].interpolation, 16);
        for (int key = 0; key < 3; ++key)
            std::memcpy(cameraBefore[key], plain.camera[key].interpolation, 24);

        Rates rates;
        rates.originalFPS = 30.0;
        rates.currentFPS = 45.0;
        const std::uint64_t untouched = plain.Digest();
        PlanPtr plan;
        Report report;
        std::wstring error;
        Check(Prepare(*plain.app, rates, plan, report, error), "the no-collision case prepares");
        CheckEqual(plain.Digest(), untouched, "the no-collision Prepare is read-only");
        CheckEqual(report.mergeGroups, 0, "the no-collision case has no collision group");
        CheckEqual(report.mergedKeys, 0, "the no-collision case merges no key");
        Check(Commit(*plain.app, plan, error, {}), "the no-collision case commits");
        for (int slot = 0; slot < 2; ++slot) {
            for (int key = 3; key <= 5; ++key) {
                char label[96];
                std::snprintf(label, sizeof(label),
                              "model %d bone key %d keeps all sixteen interpolation bytes", slot, key);
                CheckBytes(plain.boneKeys[slot][key].interpolation, boneBefore[slot][key - 3], 16, label);
            }
        }
        for (int key = 0; key < 3; ++key) {
            char label[96];
            std::snprintf(label, sizeof(label), "camera key %d keeps all twenty-four interpolation bytes", key);
            CheckBytes(plain.camera[key].interpolation, cameraBefore[key], 24, label);
        }
    }

    // ---- a collision must not refuse a legal native curve -----------------
    // 30 -> 12 collides on the bone and display chains and on the camera chain,
    // so the retiming path does read the authored curves.  An interval with
    // equal endpoint values, a crossing tuple and an end-handled tuple are all
    // legal in a record and none of them may stop the conversion.
    {
        Fixture merged;
        Build(merged);
        GiveCameraThreeKeys(merged);
        PlantNativeShapes(merged);

        std::uint8_t headBefore[2][16];
        for (int slot = 0; slot < 2; ++slot)
            std::memcpy(headBefore[slot], merged.boneKeys[slot][3].interpolation, 16);

        Rates rates;
        rates.originalFPS = 30.0;
        rates.currentFPS = 12.0;
        PlanPtr plan;
        Report report;
        std::wstring error;
        const bool prepared = Prepare(*merged.app, rates, plan, report, error);
        if (!prepared) std::fwprintf(stderr, L"Native collision preparation: %ls\n", error.c_str());
        Check(prepared,
              "a collision does not refuse an equal-valued, crossing or end-handled native curve");
        CheckEqual(report.mergeGroups, 5, "the camera and both models retain their collision groups");
        Check(Commit(*merged.app, plan, error, {}), "the colliding case commits");
        for (int slot = 0; slot < 2; ++slot) {
            char label[96];
            std::snprintf(label, sizeof(label), "model %d chain head keeps all sixteen interpolation bytes", slot);
            CheckBytes(merged.boneKeys[slot][3].interpolation, headBefore[slot], 16, label);
        }
    }

    // ---- bone scale: matrix, curve, revision and the merged slot ----------
    {
        Fixture scaled;
        Build(scaled);
        GiveCameraThreeKeys(scaled);
        unsigned char* model = scaled.Model(0);
        bs::Matrix matrix;
        matrix.v = {2.0f, 0.5f, 0.25f, 0.125f, 1.5f, 0.75f, -0.5f, 0.25f, 3.0f};
        Check(bs::Valid(matrix), "the test scale matrix is valid");
        const bs::Curve curve{20, 40, 107, 120};
        Check(!bs::Linear(curve), "the test scale curve is not linear");
        for (int slot = 3; slot <= 5; ++slot) {
            auto value = matrix;
            for (auto& component : value.v) component *= float(slot - 2);
            bs::SetKey(model, slot, value);
            bs::SetKeyCurve(model, slot, curve);
        }
        bs::CurveTableState before;
        Check(bs::CaptureCurveTable(model, before), "the scale table is captured");
        const std::uint64_t revision = bs::Revision(model);
        const bs::Matrix sampledBefore = bs::Sample(model, 3, 1.5f);
        const bs::Matrix mergedBefore = bs::Sample(model, 3, 2.5f);

        Rates rates;
        rates.originalFPS = 30.0;
        rates.currentFPS = 12.0;
        PlanPtr plan;
        Report report;
        std::wstring error;
        const std::uint64_t untouched = scaled.Digest();
        Check(Prepare(*scaled.app, rates, plan, report, error), "the scale case prepares");
        CheckEqual(scaled.Digest(), untouched, "the scale case Prepare is read-only");
        bs::CurveTableState after;
        Check(bs::CaptureCurveTable(model, after), "the scale table is read back after Prepare");
        CheckEqual(after.revision, revision, "Prepare leaves the scale revision alone");
        Check(after.curves == before.curves && after.keys == before.keys,
              "Prepare leaves the scale curves and matrices alone");
        Check(bs::Sample(model, 3, 1.5f) == sampledBefore, "Prepare leaves the scale track alone");

        Check(!Commit(*scaled.app, plan, error, [](std::wstring&) { return false; }),
              "the scale case rolls back on a refusing callback");
        bs::CurveTableState restored;
        Check(bs::CaptureCurveTable(model, restored), "the scale table is read back after rollback");
        CheckEqual(restored.revision, revision, "the rollback restores the scale revision");
        Check(restored.curves == before.curves && restored.keys == before.keys,
              "the rollback restores the scale curves and matrices");
        Check(bs::Sample(model, 3, 1.5f) == sampledBefore, "the rollback restores the scale track");

        Check(Commit(*scaled.app, plan, error, {}), "the scale case retries and succeeds");
        const auto actualMerged = bs::Key(model, 4);
        for (std::size_t component = 0; component < matrix.v.size(); ++component)
            Check(std::fabs(actualMerged.v[component] - mergedBefore.v[component]) < 1e-5,
                  "all nine merged scale components match the original sample");
        // Frames 2 and 3 collide, so slot 5 leaves the chain and must lose both
        // the matrix and the curve it carried.
        Check(bs::Identity(bs::Key(model, 5)), "the merged-away slot loses its scale matrix");
        Check(bs::KeyCurve(model, 5) == bs::LinearCurve(), "the merged-away slot loses its scale curve");
    }

    // ---- morph_curve: an authored curve survives the conversion ----------
    {
        Fixture morphed;
        Build(morphed);
        GiveCameraThreeKeys(morphed);
        unsigned char* model = morphed.Model(0);
        const int morph = 1;

        ac::Track authored;
        ac::Key first;
        first.frame = 0;
        first.value = 0.0;
        first.interpolation = ac::Interpolation::Bezier;
        first.tangent = ac::Tangent::Free;
        ac::Key last;
        last.frame = 30;
        last.value = 1.0;
        last.interpolation = ac::Interpolation::Bezier;
        last.tangent = ac::Tangent::Free;
        authored.keys.push_back(first);
        authored.keys.push_back(last);
        std::array<double, 4> domain{20, 40, 107, 120};
        std::string fit;
        Check(ac::SetMmdControls(authored, 1, domain, fit), "the authored morph curve is not linear");

        std::vector<ac::NamedTrack> tracks;
        tracks.push_back(ac::NamedTrack{
            mc::StableId(morph, morphed.morphTable[0][morph].name), authored});
        std::string writeError;
        Check(mc::Write(model, tracks, writeError), "the authored morph curve installs");
        Check(mc::AnyCurve(model), "the model reports the authored morph curve");

        ac::Track installed;
        std::string readError;
        Check(mc::Read(model, morph, installed, readError), "the authored morph curve reads back");
        const double beforeMid = ac::Evaluate(installed, 15.0).value;

        Rates rates;
        rates.originalFPS = 30.0;
        rates.currentFPS = 12.0;
        PlanPtr plan;
        Report report;
        std::wstring error;
        const std::uint64_t untouched = morphed.Digest();
        Check(Prepare(*morphed.app, rates, plan, report, error), "the authored morph case prepares");
        CheckEqual(morphed.Digest(), untouched, "the authored morph case Prepare is read-only");

        Check(!Commit(*morphed.app, plan, error, [](std::wstring&) { return false; }),
              "the authored morph case rolls back on a refusing callback");
        Check(mc::AnyCurve(model), "the rollback keeps the authored morph curve");
        ac::Track rolledBack;
        Check(mc::Read(model, morph, rolledBack, readError), "the authored morph curve reads back after rollback");
        Check(std::fabs(ac::Evaluate(rolledBack, 15.0).value - beforeMid) < 1e-10,
              "rollback restores the actual nonlinear morph sample");

        Check(Commit(*morphed.app, plan, error, {}), "the authored morph case commits");
        ac::Track retimed;
        Check(mc::Read(model, morph, retimed, readError), "the retimed authored morph curve reads back");
        // Frame 15 of the original maps to frame 6 of the converted project, so
        // the converted curve sampled at 6 must match the model's own curve
        // sampled before at 15.
        const double afterMid = ac::Evaluate(retimed, 6.0).value;
        Check(std::fabs(afterMid - beforeMid) < 0.05, "the retimed authored morph curve keeps its shape");
        Check(std::fabs(ac::Evaluate(retimed, 12.0).value - 1.0) < 0.05,
              "the retimed authored morph curve keeps its end value");
    }

    // Real native .x authoring: no MMV, rollback, stale native reconciliation,
    // and standard record values agreeing with the extension after collisions.
    {
        Fixture hosted;
        Build(hosted);
        auto& app = *hosted.app;
        std::vector<mikumikustudio::curve_editor::Channel> channels;
        std::wstring hostError;
        Check(ch::Read(app, ch::Kind::Accessory, 0, channels, hostError), "read native accessory channels");
        Check(channels.size() == 12, "all native accessory channels are available without MMV");
        if (channels.size() == 12) {
            auto& track = channels[0].track;
            track.keys[1].value = 10;
            std::string fit;
            Check(ac::SetMmdControls(track, 1, {20, 40, 107, 120}, fit), "author nonlinear accessory X");
            Check(ch::Write(app, ch::Kind::Accessory, 0, channels, hostError), "write nonlinear accessory X");
            const double midpoint = ac::Evaluate(track, 15).value;
            const auto untouched = hosted.Digest();
            PlanPtr plan;
            Report report;
            std::wstring error;
            const Rates rates{30, 12};
            Check(Prepare(app, rates, plan, report, error), "prepare authored accessory curves");
            CheckEqual(hosted.Digest(), untouched, "accessory Prepare changes no native bytes");
            Check(!Commit(app, plan, error, [](std::wstring&) { return false; }), "accessory commit can roll back");
            CheckEqual(hosted.Digest(), untouched, "accessory rollback restores native bytes");
            Check(ch::Read(app, ch::Kind::Accessory, 0, channels, hostError), "read accessory after rollback");
            Check(std::fabs(ac::Evaluate(channels[0].track, 15).value - midpoint) < 1e-10,
                  "accessory rollback restores nonlinear sampling");
            Check(Commit(app, plan, error, {}), "accessory conversion can retry");
            Check(ch::Read(app, ch::Kind::Accessory, 0, channels, hostError), "read retimed accessory");
            Check(std::fabs(ac::Evaluate(channels[0].track, 6).value - midpoint) < 1e-10,
                  "accessory nonlinear shape follows frame rate");

            // Simulate registration outside the curve window, without Read to
            // synchronise it. Prepare must reconcile a copy, not overwrite it.
            app.AccessoryKeys(0)[1].position[0] = 12;
            const auto edited = hosted.Digest();
            Check(Prepare(app, Rates{12, 24}, plan, report, error), "prepare unsynchronised native accessory edit");
            CheckEqual(hosted.Digest(), edited, "reconciliation during Prepare is read-only");
            Check(Commit(app, plan, error, {}), "commit unsynchronised native accessory edit");
            Check(ch::Read(app, ch::Kind::Accessory, 0, channels, hostError), "read reconciled accessory");
            Check(std::fabs(channels[0].track.keys.back().value - 12) < 1e-6,
                  "newly registered accessory value survives retiming");

            // Both authored interior keys map to frame 1 at 0.4x speed.
            auto& collision = channels[0].track;
            collision.keys.resize(4);
            const int frames[] = {0, 2, 3, 30};
            const double values[] = {0, 1, 7, 12};
            for (int i = 0; i < 4; ++i) {
                collision.keys[i] = ac::Key{};
                collision.keys[i].frame = frames[i];
                collision.keys[i].value = values[i];
                collision.keys[i].tangent = ac::Tangent::Free;
            }
            for (int i = 1; i < 4; ++i)
                Check(ac::SetMmdControls(collision, i, {20, 40, 107, 120}, fit), "set collision accessory curve");
            Check(ch::Write(app, ch::Kind::Accessory, 0, channels, hostError), "write accessory collision fixture");
            const double mergedValue = ac::Evaluate(collision, 2.5).value;
            Check(Prepare(app, rates, plan, report, error), "prepare accessory collision");
            Check(Commit(app, plan, error, {}), "commit accessory collision");
            Check(ch::Read(app, ch::Kind::Accessory, 0, channels, hostError), "read merged accessory curve");
            Check(std::fabs(ac::Evaluate(channels[0].track, 1).value - mergedValue) < 1e-5,
                  "merged accessory value samples the authored curve");
            const auto keySlot = app.AccessoryKeys(0)[0].next;
            Check(app.AccessoryKeys(0)[keySlot].frame == 1 &&
                  std::fabs(app.AccessoryKeys(0)[keySlot].position[0] - mergedValue) < 1e-5,
                  "standard accessory key and curve extension agree at merged frame");
        }
    }
    // A preview must never overwrite changes made while its window was open.
    for (int mutation = 0; mutation < 6; ++mutation) {
        Fixture stale;
        Build(stale);
        PlanPtr preview;
        Report report;
        std::wstring error;
        Check(Prepare(*stale.app, Rates{30, 12}, preview, report, error), "prepare stale-state fixture");
        auto& app = *stale.app;
        switch (mutation) {
        case 0: app.ModelSlot(1) = nullptr; break;
        case 1: app.AccessorySlot(2) = &stale.accessoryRecord[0]; break;
        case 2: stale.displayKeys[0][0].ikStates[0] ^= 1; break;
        case 3: {
            bs::Matrix value; value.v[0] = 2;
            bs::SetKey(stale.Model(0), 3, value);
            break;
        }
        case 4: {
            std::vector<mikumikustudio::curve_editor::Channel> channels;
            Check(ch::Read(app, ch::Kind::Accessory, 0, channels, error), "read fresh accessory authoring");
            if (!channels.empty()) {
                channels[0].track.keys.back().value = 3;
                Check(ch::Write(app, ch::Kind::Accessory, 0, channels, error), "add accessory authoring after preview");
            }
            break;
        }
        case 5: {
            ac::Track track;
            ac::Key a, b; b.frame = 30; b.value = 1;
            track.keys = {a, b};
            std::string detail;
            Check(ac::SetMmdControls(track, 1, {20, 40, 107, 120}, detail), "author fresh morph curve");
            Check(mc::Write(stale.Model(0), {{mc::StableId(1, stale.morphTable[0][1].name), track}}, detail),
                  "add morph authoring after preview");
            break;
        }
        }
        const auto edited = stale.Digest();
        bool finalized = false;
        Check(!Commit(app, preview, error, [&](std::wstring&) { finalized = true; return true; }),
              "stale object, payload or side-table state rejects Apply");
        Check(!finalized && stale.Digest() == edited, "stale rejection never writes keys or calls next backend");
    }
#endif  // MDS_NATIVE_FRAME_RETIME_STANDALONE

    if (g_failures == 0) {
        std::puts("native_frame_retime_tests: all checks passed");
        return 0;
    }
    std::printf("native_frame_retime_tests: %d failure(s)\n", g_failures);
    return 1;
}

#ifdef MDS_NATIVE_FRAME_RETIME_STANDALONE
// Stand-ins for the three side-table modules.  They answer "there is no
// authoring", which is what this suite's fixture has, so the native pools and
// the change list are exercised on their own.  The unified build links the real
// modules instead and never defines the macro.
namespace mikumikustudio::mdl {
std::size_t BoneKeyCapacity(const ModelRecord&) { return kBoneKeyCapacity; }
}  // namespace mikumikustudio::mdl

namespace mikumikustudio::bone_scale {
Matrix Key(const unsigned char*, int) { return Matrix{}; }
Curve KeyCurve(const unsigned char*, int) { return LinearCurve(); }
bool CaptureCurveTable(const unsigned char*, CurveTableState&) { return false; }
void SwapCurveTable(unsigned char*, CurveTableState&) noexcept {}
}  // namespace mikumikustudio::bone_scale

namespace mikumikustudio::morph_curve {
struct AuthoredLayers::Impl {
    int unused = 0;
};
AuthoredLayers::AuthoredLayers() : impl_(new AuthoredLayers::Impl()) {}
AuthoredLayers::~AuthoredLayers() = default;
AuthoredLayers::AuthoredLayers(AuthoredLayers&&) noexcept = default;
AuthoredLayers& AuthoredLayers::operator=(AuthoredLayers&&) noexcept = default;
bool CaptureLayers(const unsigned char*, AuthoredLayers&, std::string&) { return true; }
bool LayersTracks(const AuthoredLayers&, std::vector<animation_curve::NamedTrack>& out, std::string&) {
    out.clear();
    return true;
}
bool RebuildLayers(const unsigned char*, const std::vector<animation_curve::NamedTrack>&, AuthoredLayers&,
                   std::string&) {
    return true;
}
void SwapLayers(unsigned char*, AuthoredLayers&) noexcept {}
bool LayersUnchanged(const unsigned char*, const AuthoredLayers&) { return true; }
}  // namespace mikumikustudio::morph_curve

namespace mikumikustudio::curve_host {
struct AuthoringTable::Impl {
    int unused = 0;
};
AuthoringTable::AuthoringTable() : impl_(new AuthoringTable::Impl()) {}
AuthoringTable::~AuthoringTable() = default;
AuthoringTable::AuthoringTable(AuthoringTable&&) noexcept = default;
AuthoringTable& AuthoringTable::operator=(AuthoringTable&&) noexcept = default;
bool CaptureAuthoring(MMDApp&, AuthoringTable&, std::wstring&) { return true; }
bool AuthoringEntries(const AuthoringTable&, std::vector<AuthoringEntry>& out) {
    out.clear();
    return true;
}
bool SampleRetimedKey(const AuthoringEntry&, void*, std::size_t) { return false; }
bool RebuildAuthoring(MMDApp&, const std::vector<AuthoringEntry>&, AuthoringTable&, std::wstring&) {
    return true;
}
void SwapAuthoring(MMDApp&, AuthoringTable&) noexcept {}
bool AuthoringUnchanged(MMDApp&, const AuthoringTable&) { return true; }
}  // namespace mikumikustudio::curve_host
#endif
