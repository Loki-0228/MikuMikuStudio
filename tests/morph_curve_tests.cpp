// MAINTENANCE: regression-keep; native morph (expression) curves
// (include/mikumikustudio/morph_curve.hpp, src/animation/morph_curve.cpp).
//
// Covered here, all on the CPU and without a window, a GPU or a real model:
//   * enumeration: index, panel, stable id, display name, and byte-exact
//     preservation of a non-ASCII name through the id roundtrip,
//   * native read: the model's MorphKey chain as a linear animation_curve track,
//   * no authoring: Evaluate is the identity and leaves every morph value alone,
//   * authored write: the native mirror, exact per-frame sampling, frame zero,
//     transactional rejection and untouched other morphs,
//   * native edits: registration, value change and deletion drop the authored
//     curve where they touch, so a stale curve can never stay in force,
//   * model isolation and identity: a second model, a disposed model and a
//     swapped morph table never inherit another model's curves,
//   * PMM roundtrip through the real pmm_extension document, including the
//     drop of a record whose model no longer matches,
//   * VMD roundtrip through the real block codec, the standard VMD prefix
//     staying readable for a standard-section reader (curve_motion), and the
//     bone-scale extension container still validating with the block in front
//     of it (its prefix fingerprint and boundary are checked by the real
//     bone_scale_io codec).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mikumikustudio/animation_curve_motion.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/animation_curve_host.hpp"

namespace mc = mikumikustudio::morph_curve;
namespace ac = mikumikustudio::animation_curve;
namespace mdl = mikumikustudio::mdl;
namespace bsio = mikumikustudio::bone_scale_io;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

bool Near(double left, double right, double tolerance = 1e-6) {
    return std::fabs(left - right) <= tolerance;
}

// ---------------------------------------------------------------------------
// A model block with a morph table and a morph key pool, and nothing else:
// the module under test only ever looks at those two.
// ---------------------------------------------------------------------------

struct FakeModel {
    std::unique_ptr<mdl::ModelRecord> storage;
    std::vector<mdl::MorphRecord> morphs;
    std::vector<mdl::MorphKey> keys;

    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(storage.get()); }
    mdl::ModelRecord* record() const { return storage.get(); }
    mdl::MorphRecord* morphTable() { return morphs.data(); }
    float& value(int morph) { return morphs[static_cast<std::size_t>(morph)].value; }
    float value(int morph) const { return morphs[static_cast<std::size_t>(morph)].value; }
};

void SetName(mdl::MorphRecord& morph, const std::string& jp, const std::string& en) {
    std::memcpy(morph.name, jp.data(), jp.size() < 20 ? jp.size() : 20);
    std::memcpy(morph.nameEn, en.data(), en.size() < 20 ? en.size() : 20);
}

FakeModel MakeModel(const std::vector<std::pair<std::string, std::uint8_t>>& definitions,
                    const std::string& modelName = "morph-curve-test") {
    FakeModel model;
    model.storage = std::make_unique<mdl::ModelRecord>();
    model.morphs.assign(definitions.size(), mdl::MorphRecord{});
    model.keys.assign(mdl::kMorphKeyCapacity, mdl::MorphKey{});
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        mdl::MorphRecord& morph = model.morphs[index];
        morph.type = 1;  // vertex morph: nothing here applies morph types
        morph.panel = static_cast<mdl::MorphPanel>(definitions[index].second);
        SetName(morph, definitions[index].first, definitions[index].first);
    }
    std::memcpy(model.storage->name, modelName.data(),
                modelName.size() < 20 ? modelName.size() : 20);
    model.storage->morphCount = static_cast<std::uint32_t>(definitions.size());
    model.storage->morphs = model.morphs.data();
    model.storage->morphKeys = model.keys.data();
    return model;
}

int AllocSlot(FakeModel& model) {
    for (int index = static_cast<int>(model.storage->morphCount);
         index < static_cast<int>(mdl::kMorphKeyCapacity); ++index) {
        if (model.keys[static_cast<std::size_t>(index)].frame == 0) return index;
    }
    return -1;
}

// Mimics the registrar: ascending chain, allocated marks, max frame bump.
void NativeAdd(FakeModel& model, int morph, int frame, float value) {
    mdl::MorphKey* keys = model.keys.data();
    int current = morph;
    while (keys[current].next != 0 && static_cast<int>(keys[keys[current].next].frame) < frame) {
        current = static_cast<int>(keys[current].next);
    }
    const int next = static_cast<int>(keys[current].next);
    if (next != 0 && static_cast<int>(keys[next].frame) == frame) {
        keys[next].value = value;
        keys[next].allocated = 1;
    } else {
        const int fresh = AllocSlot(model);
        if (fresh < 0) std::abort();
        keys[fresh].frame = static_cast<std::uint32_t>(frame);
        keys[fresh].value = value;
        keys[fresh].allocated = 1;
        keys[fresh].previous = static_cast<std::uint32_t>(current);
        keys[fresh].next = static_cast<std::uint32_t>(next);
        keys[current].next = static_cast<std::uint32_t>(fresh);
        if (next != 0) keys[next].previous = static_cast<std::uint32_t>(fresh);
    }
    if (static_cast<std::uint32_t>(frame) > model.storage->maxFrame) {
        model.storage->maxFrame = static_cast<std::uint32_t>(frame);
    }
}

void NativeTrack(FakeModel& model, int morph, const std::vector<std::pair<int, float>>& track) {
    model.keys[static_cast<std::size_t>(morph)] = mdl::MorphKey{};
    model.keys[static_cast<std::size_t>(morph)].frame = 0;
    model.keys[static_cast<std::size_t>(morph)].allocated = 1;
    for (std::size_t index = 0; index < track.size(); ++index) {
        if (index == 0) {
            if (track[index].first == 0) {
                model.keys[static_cast<std::size_t>(morph)].value = track[index].second;
                continue;
            }
            model.keys[static_cast<std::size_t>(morph)].value = 0.0f;
        }
        NativeAdd(model, morph, track[index].first, track[index].second);
    }
}

bool NativeRemove(FakeModel& model, int morph, int frame) {
    mdl::MorphKey* keys = model.keys.data();
    int current = morph;
    while (keys[current].next != 0) {
        const int next = static_cast<int>(keys[current].next);
        if (static_cast<int>(keys[next].frame) == frame) {
            const int after = static_cast<int>(keys[next].next);
            keys[current].next = static_cast<std::uint32_t>(after);
            if (after != 0) keys[after].previous = static_cast<std::uint32_t>(current);
            keys[next] = mdl::MorphKey{};
            return true;
        }
        current = next;
    }
    return false;
}

std::vector<mdl::MorphKey> PoolSnapshot(const FakeModel& model) { return model.keys; }

bool PoolUnchanged(const FakeModel& model, const std::vector<mdl::MorphKey>& snapshot) {
    if (snapshot.size() != model.keys.size()) return false;
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        const mdl::MorphKey& left = snapshot[index];
        const mdl::MorphKey& right = model.keys[index];
        if (left.frame != right.frame || left.value != right.value ||
            left.allocated != right.allocated || left.previous != right.previous ||
            left.next != right.next) {
            return false;
        }
    }
    return true;
}

ac::Track Curve(std::initializer_list<std::pair<int, double>> keys) {
    ac::Track track;
    for (const auto& key : keys) {
        ac::Key entry;
        entry.frame = key.first;
        entry.value = key.second;
        entry.inSlope = entry.outSlope = 0.0;
        entry.inWeight = entry.outWeight = 1.0 / 3.0;
        entry.interpolation = ac::Interpolation::Linear;
        entry.tangent = ac::Tangent::Free;
        track.keys.push_back(entry);
    }
    return track;
}

ac::NamedTrack Named(const FakeModel& model, int morph, ac::Track track) {
    ac::NamedTrack named;
    named.id = mc::StableId(morph, model.morphs[static_cast<std::size_t>(morph)].name);
    named.track = std::move(track);
    return named;
}

// A two-key interval with a deliberately strong ease: both handle abscissae
// stay at the default 1/3, the out slope of the first key is flat and the in
// slope of the second key pulls the second control point down to the start
// value, so the curve sits near the start value until late in the interval.
// Built by hand instead of through the MMD control grid so the test does not
// depend on which control bytes that grid accepts.
ac::Track EasedCurve() {
    ac::Track track = Curve({{0, 0.0}, {30, 1.0}});
    track.keys[0].outSlope = 0.0;
    track.keys[1].interpolation = ac::Interpolation::Bezier;
    track.keys[1].tangent = ac::Tangent::Free;
    track.keys[1].inSlope = 0.1;
    return track;
}

// Turns one interval of a curve into the same strong ease EasedCurve uses.
void EaseInto(ac::Track& track, std::size_t endKey, double inSlope) {
    track.keys[endKey].interpolation = ac::Interpolation::Bezier;
    track.keys[endKey].tangent = ac::Tangent::Free;
    track.keys[endKey].inSlope = inSlope;
}

// Sets the export mark ("selected" in the MMD timeline) of one native record.
bool NativeSetMark(FakeModel& model, int morph, int frame, std::uint8_t allocated) {
    mdl::MorphKey* keys = model.keys.data();
    int current = morph;
    for (std::size_t visited = 0; visited <= mdl::kMorphKeyCapacity; ++visited) {
        if (static_cast<int>(keys[current].frame) == frame) {
            keys[current].allocated = allocated;
            return true;
        }
        if (keys[current].next == 0) return false;
        current = static_cast<int>(keys[current].next);
    }
    return false;
}

void NativeClearMarks(FakeModel& model, int morph) {
    mdl::MorphKey* keys = model.keys.data();
    int current = morph;
    for (std::size_t visited = 0; visited <= mdl::kMorphKeyCapacity; ++visited) {
        keys[current].allocated = 0;
        if (keys[current].next == 0) return;
        current = static_cast<int>(keys[current].next);
    }
}

// Bit-for-bit comparison, so a restored authored curve is proven to be the very
// same curve and not merely one with equal samples.
bool SameCurve(const ac::Track& left, const ac::Track& right) {
    if (left.discrete != right.discrete || left.keys.size() != right.keys.size()) return false;
    for (std::size_t index = 0; index < left.keys.size(); ++index) {
        const ac::Key& a = left.keys[index];
        const ac::Key& b = right.keys[index];
        if (a.frame != b.frame || a.value != b.value || a.inSlope != b.inSlope ||
            a.outSlope != b.outSlope || a.inWeight != b.inWeight || a.outWeight != b.outWeight ||
            a.interpolation != b.interpolation || a.tangent != b.tangent) {
            return false;
        }
    }
    return true;
}

// The tracks one VMD block carries, for inspection.
std::vector<ac::NamedTrack> BlockTracks(const std::vector<std::uint8_t>& block) {
    std::vector<std::uint8_t> payload;
    std::vector<ac::NamedTrack> tracks;
    std::string error;
    if (mc::DecodeVmdBlock(block.data(), block.size(), payload) != block.size()) return tracks;
    if (!mc::DecodePayload(payload.data(), payload.size(), tracks, error)) tracks.clear();
    return tracks;
}

// The block a save would write for one hand-built track list.
std::vector<std::uint8_t> BlockOf(const std::vector<ac::NamedTrack>& tracks) {
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> block;
    std::string error;
    if (!mc::EncodePayload(tracks, payload, error)) return block;
    mc::EncodeVmdBlock(payload, block, error);
    return block;
}

// The exact probe src/model/vmd_load.cpp performs on the bytes that follow a
// standard body: read the sixteen header bytes, ask the header how big the block
// is, then read and decode that many bytes.  Pinning it here is what keeps the
// loader's contract honest - a header-only probe has to report the declared size
// (the header cannot know how much of the file is left), and the body has to be
// decoded from the whole block rather than from that probe.
bool ProbeBlockLikeLoader(const std::vector<std::uint8_t>& stream, std::size_t at,
                          std::vector<std::uint8_t>& block) {
    block.clear();
    if (at > stream.size() || stream.size() - at < 16) return false;
    const std::uint8_t* bytes = stream.data() + at;
    const std::size_t total = mc::VmdBlockSize(bytes, 16);
    if (total < 16 || total > stream.size() - at) return false;
    std::vector<std::uint8_t> payload;
    if (mc::DecodeVmdBlock(bytes, total, payload) != total) return false;
    block.assign(bytes, bytes + total);
    return true;
}

// ---------------------------------------------------------------------------
// Temporary motion file, as in the bone-scale codec test.
// ---------------------------------------------------------------------------

class TempMotion {
public:
    TempMotion() {
        wchar_t directory[MAX_PATH] = {}, path[MAX_PATH] = {};
        if (!GetTempPathW(MAX_PATH, directory) ||
            !GetTempFileNameW(directory, L"mcv", 0, path))
            std::abort();
        path_ = path;
    }
    ~TempMotion() { DeleteFileW(path_.c_str()); }
    const std::wstring& path() const { return path_; }
    int Open(int mode) const {
        int fd = -1;
        if (_wsopen_s(&fd, path_.c_str(), mode | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
            std::abort();
        return fd;
    }
    bool Write(const std::vector<std::uint8_t>& bytes) const {
        const int fd = Open(_O_WRONLY | _O_TRUNC);
        const bool okay =
            _write(fd, bytes.data(), static_cast<unsigned>(bytes.size())) ==
            static_cast<int>(bytes.size());
        return _close(fd) == 0 && okay;
    }
    std::vector<std::uint8_t> Read() const {
        const int fd = Open(_O_RDONLY);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(_filelengthi64(fd)));
        if (!bytes.empty() &&
            _read(fd, bytes.data(), static_cast<unsigned>(bytes.size())) !=
                static_cast<int>(bytes.size()))
            std::abort();
        _close(fd);
        return bytes;
    }

private:
    std::wstring path_;
};

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void PutF32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
}

// A complete standard VMD body: header, model name, empty bone section, the
// given morph records, then empty camera/light/self-shadow/IK sections.
std::vector<std::uint8_t> StandardVmd(
    const std::vector<std::tuple<std::string, int, float>>& morphs) {
    std::vector<std::uint8_t> bytes;
    const char header[30] = "Vocaloid Motion Data 0002";
    bytes.insert(bytes.end(), header, header + 30);
    std::uint8_t name[20] = {};
    const char model[] = "curvetest";
    std::memcpy(name, model, sizeof(model) - 1);
    bytes.insert(bytes.end(), name, name + 20);
    PutU32(bytes, 0);
    PutU32(bytes, static_cast<std::uint32_t>(morphs.size()));
    for (const auto& record : morphs) {
        std::uint8_t field[15] = {};
        const std::string& text = std::get<0>(record);
        std::memcpy(field, text.data(), text.size() < 15 ? text.size() : 15);
        bytes.insert(bytes.end(), field, field + 15);
        PutU32(bytes, static_cast<std::uint32_t>(std::get<1>(record)));
        PutF32(bytes, std::get<2>(record));
    }
    PutU32(bytes, 0);  // camera
    PutU32(bytes, 0);  // light
    PutU32(bytes, 0);  // self-shadow
    PutU32(bytes, 0);  // display / IK
    return bytes;
}

// ---------------------------------------------------------------------------
// Enumeration and identity
// ---------------------------------------------------------------------------

void EnumerationAndIds() {
    // CP932 "あ", ASCII, CP932 "口パク", a Chinese CP936 name and an empty name.
    const std::string japanese = "\x82\xa0";
    const std::string chinese = "\xd6\xd0\xce\xc4";
    FakeModel model = MakeModel({{"eye", 2}, {japanese, 1}, {"mouth", 3}, {chinese, 4}, {"", 0}});

    const std::vector<mc::MorphInfo> info = mc::Enumerate(model.raw(), 1);
    Check(info.size() == 5, "enumeration returns every morph");
    Check(info[0].index == 0 && info[0].panel == 2, "eye panel and index");
    Check(info[1].panel == 1 && info[3].panel == 4 && info[4].panel == 0, "panels are read per morph");
    Check(!info[0].label.empty() && info[0].label == L"eye", "English display name");
    Check(mc::PanelLabel(1, 0) != nullptr && mc::PanelLabel(1, 1) != nullptr &&
              mc::PanelLabel(1, 2) != nullptr,
          "panel labels exist for all three languages");

    // The identity must survive any byte pattern, which is what keeps a Chinese
    // or Japanese name intact across a save.
    for (const mc::MorphInfo& entry : info) {
        int index = -1;
        std::string raw;
        Check(mc::ParseStableId(entry.id, index, raw), "stable id parses");
        Check(index == entry.index && raw.size() == 20, "stable id keeps the morph index");
        Check(std::memcmp(raw.data(), model.morphs[static_cast<std::size_t>(entry.index)].name,
                          20) == 0,
              "stable id keeps the twenty raw name bytes");
    }
    Check(mc::Enumerate(nullptr).empty(), "a null model enumerates nothing");
    Check(mc::Enumerate(MakeModel({}).raw()).empty(), "a model without morphs enumerates nothing");
}

// ---------------------------------------------------------------------------
// Native read and the no-authoring identity
// ---------------------------------------------------------------------------

void NativeReadAndIdentity() {
    FakeModel model = MakeModel({{"eye", 2}, {"mouth", 3}});
    NativeTrack(model, 0, {{0, 0.0f}, {20, 1.0f}});
    NativeTrack(model, 1, {{0, 0.5f}});

    ac::Track track;
    std::string error;
    Check(mc::ReadNative(model.raw(), 0, track, error), "native read succeeds");
    Check(track.keys.size() == 2 && track.keys[0].frame == 0 && track.keys[1].frame == 20,
          "native read keeps every record");
    Check(track.keys[1].interpolation == ac::Interpolation::Linear,
          "native morph keys are linear");
    Check(mc::ReadNative(model.raw(), 9, track, error) == false, "an out-of-range morph is refused");

    Check(!mc::AnyCurve(model.raw()) && !mc::HasCurve(model.raw(), 0) &&
              mc::CurveCount(model.raw()) == 0,
          "a fresh model has no authoring");
    Check(mc::Read(model.raw(), 0, track, error), "plain read falls back to the native track");
    Check(track.keys.size() == 2 && track.keys[1].value == 1.0, "plain read returns the native track");

    // Without authoring, Evaluate must not touch a single morph value.
    mdl::Morphs(model.raw())[0].value = 0.25f;
    mdl::Morphs(model.raw())[1].value = 0.75f;
    mc::Evaluate(model.raw(), 10.0);
    Check(model.value(0) == 0.25f && model.value(1) == 0.75f, "Evaluate is the identity without a curve");
}

// ---------------------------------------------------------------------------
// Authored write, sampling, transactional rejection and untouched morphs
// ---------------------------------------------------------------------------

void WriteSamplingAndRejection() {
    FakeModel model = MakeModel({{"eye", 2}, {"mouth", 3}});
    NativeTrack(model, 0, {{0, 0.0f}});
    NativeTrack(model, 1, {{0, 0.5f}});

    // A deliberately eased interval: the exact value at frame 15 must differ
    // from the native linear interpolation, or the test proves nothing.
    ac::Track eased = EasedCurve();
    std::string error;
    const double exactAt15 = ac::Evaluate(eased, 15.0).value;
    Check(Near(exactAt15, 0.125, 1e-6), "the eased curve evaluates to its designed value");
    Check(std::fabs(exactAt15 - 0.5) > 0.05, "the eased curve is not the linear one");

    Check(mc::Write(model.raw(), {Named(model, 0, eased)}, error), "authored write succeeds");
    Check(mc::HasCurve(model.raw(), 0) && mc::AnyCurve(model.raw()) &&
              mc::CurveCount(model.raw()) == 1,
          "authoring is recorded for the written morph only");
    Check(!mc::HasCurve(model.raw(), 1), "an unwritten morph gains no authoring");

    // Native mirror: one key per curve key, frame zero kept, values as written.
    const mdl::MorphKey* keys = model.keys.data();
    Check(keys[0].frame == 0 && keys[0].value == 0.0f && keys[0].allocated == 1,
          "the native head keeps frame zero and the curve value");
    const int second = static_cast<int>(keys[0].next);
    Check(second > 0 && keys[second].frame == 30 && keys[second].value == 1.0f &&
              keys[second].allocated == 1 && keys[second].next == 0,
          "the native mirror holds the second curve key");
    Check(keys[1].frame == 0 && keys[1].value == 0.5f && keys[1].next == 0,
          "an unwritten morph track is untouched");

    // Exact evaluation, and frame zero preserved.
    mdl::Morphs(model.raw())[0].value = 0.0f;
    mc::Evaluate(model.raw(), 15.0);
    Check(Near(model.value(0), exactAt15, 1e-6), "the exact curve drives the live weight");
    Check(std::fabs(model.value(0) - 0.5) > 0.05, "the native linear value is not what plays");
    mc::Evaluate(model.raw(), 0.0);
    Check(model.value(0) == 0.0f, "frame zero keeps the authored value");
    Check(mc::Read(model.raw(), 0, eased, error) && eased.keys.size() == 2 &&
              Near(eased.keys[1].value, 1.0),
          "reading back returns the authored curve");

    // Transactional rejection: every failure leaves the pool and the authoring
    // exactly as they were.
    const std::vector<mdl::MorphKey> afterWrite = PoolSnapshot(model);
    ac::NamedTrack noZero = Named(model, 0, Curve({{10, 0.5}, {30, 1.0}}));
    Check(!mc::Write(model.raw(), {noZero}, error), "a track without a frame-zero key is refused");
    Check(PoolUnchanged(model, afterWrite), "the refused write left the pool alone");

    ac::NamedTrack wrongMorph = Named(model, 0, Curve({{0, 0.0}, {30, 1.0}}));
    wrongMorph.id = mc::StableId(1, model.morphs[0].name);
    Check(!mc::Write(model.raw(), {wrongMorph}, error), "an id that names another morph is refused");
    Check(!mc::Write(model.raw(), {Named(model, 0, Curve({}))}, error),
          "an empty track is refused");
    Check(!mc::Write(model.raw(), {}, error), "an empty write is refused");
    Check(PoolUnchanged(model, afterWrite), "every refused write left the pool alone");
    Check(mc::HasCurve(model.raw(), 0) && mc::CurveCount(model.raw()) == 1,
          "a refused write keeps the existing authoring");

    // A morph track that is not in the write is replaced only for the morph it
    // names: writing morph 1 leaves morph 0's mirror alone.
    const std::vector<mdl::MorphKey> afterSecond = PoolSnapshot(model);
    Check(mc::Write(model.raw(), {Named(model, 1, Curve({{0, 0.25}, {12, 0.75}}))}, error),
          "a second morph can be authored");
    Check(keys[0].frame == 0 && keys[0].value == 0.0f, "the first morph mirror survives");
    Check(keys[1].frame == 0 && keys[1].value == 0.25f && !PoolUnchanged(model, afterSecond),
          "the second morph mirror is rewritten");
}

// ---------------------------------------------------------------------------
// Native edits invalidate the authored curve where they touch
// ---------------------------------------------------------------------------

void NativeEditsInvalidate() {
    FakeModel model = MakeModel({{"eye", 2}});
    NativeTrack(model, 0, {{0, 0.0f}});
    ac::Track eased = EasedCurve();
    std::string error;
    const double authoredAt15 = ac::Evaluate(eased, 15.0).value;
    Check(mc::Write(model.raw(), {Named(model, 0, eased)}, error), "authoring installed");

    // Ordinary keyframe registration at a frame the curve did not have.
    NativeAdd(model, 0, 10, 0.9f);
    ac::Track track;
    Check(mc::Read(model.raw(), 0, track, error), "read after a native registration");
    Check(track.keys.size() == 3, "the registered frame joins the authored curve");
    Check(track.keys[1].frame == 10 && Near(track.keys[1].value, 0.9), "the native value wins");
    Check(track.keys[1].interpolation == ac::Interpolation::Linear &&
              track.keys[2].interpolation == ac::Interpolation::Linear,
          "the authored ease of the touched interval is dropped");

    mc::Evaluate(model.raw(), 15.0);
    const double expected = 0.9 + (1.0 - 0.9) * (5.0 / 20.0);
    Check(Near(model.value(0), expected, 1e-6), "playback follows the native linear track again");
    Check(std::fabs(model.value(0) - authoredAt15) > 0.01, "the stale curve value is gone");

    // Ordinary keyframe deletion.
    Check(NativeRemove(model, 0, 30), "the native key at frame 30 is removed");
    Check(mc::Read(model.raw(), 0, track, error) && track.keys.size() == 2,
          "the authored key whose native frame disappeared is dropped");
    mc::Evaluate(model.raw(), 40.0);
    Check(Near(model.value(0), 0.9, 1e-6), "the deleted key no longer extends the curve");

    // An ordinary value change at an existing frame.
    NativeAdd(model, 0, 10, 0.2f);
    Check(mc::Read(model.raw(), 0, track, error) && Near(track.keys[1].value, 0.2),
          "a native value change replaces the authored value");
    mc::Evaluate(model.raw(), 10.0);
    Check(Near(model.value(0), 0.2, 1e-6), "the changed native value plays");
}

// ---------------------------------------------------------------------------
// Model isolation, disposal and identity
// ---------------------------------------------------------------------------

void ModelIsolationAndIdentity() {
    FakeModel first = MakeModel({{"eye", 2}});
    FakeModel second = MakeModel({{"eye", 2}}, "other");
    NativeTrack(first, 0, {{0, 0.0f}});
    NativeTrack(second, 0, {{0, 0.0f}});
    std::string error;
    Check(mc::Write(first.raw(), {Named(first, 0, Curve({{0, 0.1}, {10, 0.2}}))}, error),
          "first model authored");
    Check(mc::Write(second.raw(), {Named(second, 0, Curve({{0, 0.8}, {10, 0.9}}))}, error),
          "second model authored");

    mc::Evaluate(first.raw(), 5.0);
    mc::Evaluate(second.raw(), 5.0);
    Check(Near(first.value(0), 0.15, 1e-6) && Near(second.value(0), 0.85, 1e-6),
          "each model plays its own curve");

    mc::Forget(first.raw());
    Check(!mc::AnyCurve(first.raw()) && mc::AnyCurve(second.raw()),
          "disposing one model leaves the other intact");
    first.value(0) = 0.33f;
    mc::Evaluate(first.raw(), 5.0);
    Check(first.value(0) == 0.33f, "the disposed model is no longer overridden");

    // A different morph table at the same address must not inherit the curves.
    second.morphs[0].name[0] = 'X';
    second.storage->morphs = second.morphs.data();
    Check(!mc::AnyCurve(second.raw()), "a swapped morph table fails the identity check");
    second.value(0) = 0.44f;
    mc::Evaluate(second.raw(), 5.0);
    Check(second.value(0) == 0.44f, "no curve is applied to the swapped model");

    // An offline copy (the physics bake model copy) carries its own authoring.
    // src/physics/physics_bake.cpp ModelCopy copies the record but leaves
    // m.morphKeys pointing at the source array, so a faithful fixture gives the
    // destination the same pool content; CopyState then derives the copy's
    // effective layer from the chain the copy actually has.
    FakeModel copy = MakeModel({{"eye", 2}}, "copy");
    mc::CopyState(first.raw(), copy.raw());
    Check(!mc::AnyCurve(copy.raw()), "a source without authoring copies nothing");
    mc::CopyState(second.raw(), copy.raw());
    Check(!mc::AnyCurve(copy.raw()), "a swapped source copies nothing");
    mc::Forget(copy.raw());
    FakeModel third = MakeModel({{"eye", 2}}, "third");
    NativeTrack(third, 0, {{0, 0.0f}});
    std::string error2;
    Check(mc::Write(third.raw(), {Named(third, 0, Curve({{0, 0.2}, {10, 0.6}}))}, error2),
          "third model authored");
    copy.keys = third.keys;                      // the shared native pool of a real copy
    copy.storage->morphKeys = copy.keys.data();
    mc::CopyState(third.raw(), copy.raw());
    Check(mc::AnyCurve(copy.raw()), "the copy receives the authoring");
    mc::Evaluate(copy.raw(), 5.0);
    Check(Near(copy.value(0), 0.4, 1e-6), "the copy plays the same curve");
    mc::Forget(third.raw());
    Check(mc::AnyCurve(copy.raw()), "dropping the source leaves the copy alone");
}

// ---------------------------------------------------------------------------
// The export mark is not occupancy: an unmarked key still exists
// ---------------------------------------------------------------------------

void NativeMarksAreNotOccupancy() {
    FakeModel model = MakeModel({{"eye", 2}, {"mouth", 3}});
    NativeTrack(model, 0, {{0, 0.0f}});
    NativeAdd(model, 0, 20, 0.8f);   // a real key record at frame 20
    NativeTrack(model, 1, {{0, 0.5f}});
    std::string error;

    // (1) Unmarking everything does not make the track empty: the records are
    //     still there, only their export mark is gone.
    NativeClearMarks(model, 0);
    const std::vector<mc::MorphInfo> info = mc::Enumerate(model.raw(), 1);
    Check(info.size() == 2 && info[0].keyed && !info[0].marked,
          "an unmarked track is still a keyed track");
    Check(info[1].keyed && info[1].marked, "a marked track reports both flags");
    ac::Track read;
    Check(mc::ReadNative(model.raw(), 0, read, error) && read.keys.size() == 2 &&
              read.keys[1].frame == 20,
          "an unmarked key is still read as a key");

    // (2) Applying a curve selects the keys it writes: the standard section and
    //     the curve block carry the same frames, so an unmarked one would be
    //     lost from a VMD roundtrip.
    ac::Track curve = Curve({{0, 0.0}, {20, 0.8}, {40, 1.0}});
    Check(mc::Write(model.raw(), {Named(model, 0, curve)}, error), "curve installed over the marks");
    const mdl::MorphKey* keys = model.keys.data();
    int slot20 = -1;
    int slot40 = -1;
    for (int index = static_cast<int>(model.storage->morphCount);
         index < static_cast<int>(mdl::kMorphKeyCapacity); ++index) {
        if (static_cast<int>(keys[index].frame) == 20) slot20 = index;
        if (static_cast<int>(keys[index].frame) == 40) slot40 = index;
    }
    Check(keys[0].frame == 0 && keys[0].allocated == 1, "the frame-zero key is selected");
    Check(slot20 >= 0 && keys[slot20].allocated == 1 && keys[slot20].value == 0.8f,
          "a mirrored frame is selected for export");
    Check(slot40 >= 0 && keys[slot40].allocated == 1, "a key the write created is selected");

    // (3) The export follows the marks, so unmarking a key removes it from both
    //     the standard section and the block, and nothing else changes.
    NativeClearMarks(model, 0);
    Check(NativeSetMark(model, 0, 40, 1), "mark only frame 40");
    std::vector<std::uint8_t> block;
    Check(mc::CaptureVmd(model.raw(), 0, block, error) && !block.empty(),
          "the marked frames export");
    const std::vector<ac::NamedTrack> tracks = BlockTracks(block);
    Check(tracks.size() == 1 && tracks[0].track.keys.size() == 1,
          "the unmarked frames are not exported");
    Check(tracks[0].track.keys[0].frame == 40, "the exported frame is the marked one");

    // (4) Occupancy is the frame, not the mark: the free-slot scan must not hand
    //     an unmarked record to another morph.
    Check(mc::Write(model.raw(), {Named(model, 1, Curve({{0, 0.5}, {10, 0.6}, {30, 0.7}}))},
                    error),
          "a second morph can be written");
    Check(keys[slot20].frame == 20 && keys[slot20].value == 0.8f &&
              keys[slot20].allocated == 0,
          "the unmarked record is not recycled as a free slot");

    // (5) Mark churn is not a data edit: unmarking records leaves the curve and
    //     its sampling alone.
    ac::Track before;
    ac::Track after;
    Check(mc::Read(model.raw(), 0, before, error), "read after the writes");
    NativeClearMarks(model, 0);
    Check(mc::Read(model.raw(), 0, after, error) && SameCurve(before, after),
          "unmarking every key does not change the curve");
    Check(Near(ac::Evaluate(after, 30.0).value, ac::Evaluate(before, 30.0).value, 1e-9),
          "unmarking does not change what the curve samples");
}

// ---------------------------------------------------------------------------
// The pristine layer survives an edit: a restored pool restores the tangents
// ---------------------------------------------------------------------------

void AuthoredFallbackAndRestore() {
    FakeModel model = MakeModel({{"eye", 2}});
    NativeTrack(model, 0, {{0, 0.0f}});
    const ac::Track authored = EasedCurve();
    std::string error;
    Check(mc::Write(model.raw(), {Named(model, 0, authored)}, error), "eased curve installed");

    ac::Track read;
    Check(mc::Read(model.raw(), 0, read, error) && SameCurve(read, authored),
          "a freshly installed curve reads back unchanged");
    mc::Evaluate(model.raw(), 15.0);
    Check(Near(model.value(0), 0.125, 1e-6), "the authored ease drives playback");

    // An ordinary registration bends the effective curve only: the effective
    // layer goes linear around the edit while the author's data stays put.
    NativeAdd(model, 0, 10, 0.9f);
    mc::Evaluate(model.raw(), 15.0);
    Check(Near(model.value(0), 0.925, 1e-6), "the native key takes over while it exists");
    Check(mc::Read(model.raw(), 0, read, error) && read.keys.size() == 3 &&
              read.keys[2].interpolation == ac::Interpolation::Linear,
          "the effective curve falls back to linear where the edit landed");

    // Deleting that key puts the pool back the way the curve was installed, so
    // the authored tangents are in force again - with no undo hook anywhere.
    Check(NativeRemove(model, 0, 10), "the extra key is removed");
    Check(mc::Read(model.raw(), 0, read, error) && SameCurve(read, authored),
          "the authored curve comes back bit for bit");
    mc::Evaluate(model.raw(), 15.0);
    Check(Near(model.value(0), 0.125, 1e-6), "the authored ease plays again");

    // A value change at an existing authored frame is the same story.
    NativeAdd(model, 0, 30, 0.7f);
    mc::Evaluate(model.raw(), 15.0);
    Check(Near(model.value(0), 0.35, 1e-6), "a changed native value takes over");
    Check(mc::Read(model.raw(), 0, read, error) && read.keys.size() == 2 &&
              read.keys[1].interpolation == ac::Interpolation::Linear,
          "the changed frame drops the authored ease");
    NativeAdd(model, 0, 30, 1.0f);
    Check(mc::Read(model.raw(), 0, read, error) && SameCurve(read, authored),
          "restoring the value restores the authored curve");
    mc::Evaluate(model.raw(), 15.0);
    Check(Near(model.value(0), 0.125, 1e-6), "the restored curve plays the authored ease");

    // Losing the last mirrored key leaves only the frame-zero anchor in force,
    // and the authoring is still retained: re-adding the key brings it all back.
    Check(NativeRemove(model, 0, 30), "the last interval key is removed");
    Check(mc::Read(model.raw(), 0, read, error) && read.keys.size() == 1 &&
              read.keys[0].frame == 0,
          "only the frame-zero anchor is left in force");
    Check(mc::AnyCurve(model.raw()), "the pristine authoring is retained while reduced");
    NativeAdd(model, 0, 30, 1.0f);
    Check(mc::Read(model.raw(), 0, read, error) && SameCurve(read, authored),
          "the retained authoring returns with the pool");
    mc::Evaluate(model.raw(), 15.0);
    Check(Near(model.value(0), 0.125, 1e-6), "the returned curve plays the authored ease");
}

// ---------------------------------------------------------------------------
// PMM roundtrip through the real extension document
// ---------------------------------------------------------------------------

void PmmRoundTrip() {
    mikumikustudio::MMDApp app;
    FakeModel first = MakeModel({{"eye", 2}, {"mouth", 3}});
    FakeModel second = MakeModel({{"eye", 2}}, "other");
    app.ModelSlot(0) = first.raw();
    app.ModelSlot(1) = second.raw();
    NativeTrack(first, 0, {{0, 0.0f}});
    NativeTrack(second, 0, {{0, 0.0f}});
    std::string error;
    Check(mc::Write(first.raw(), {Named(first, 0, Curve({{0, 0.0}, {24, 1.0}}))}, error),
          "slot 0 authored");
    Check(mc::Write(second.raw(), {Named(second, 0, Curve({{0, 0.5}, {12, 0.25}}))}, error),
          "slot 1 authored");

    std::string slot = mc::PmmKey(3);
    int parsedSlot = -1;
    std::string version;
    Check(mc::ParsePmmKey(slot, parsedSlot, version) && parsedSlot == 3 && version == "v1",
          "the PMM key parses back to its slot and version");
    Check(!mc::ParsePmmKey("org.mikudancestudio.model.3.boneScale.v1", parsedSlot, version),
          "the bone-scale key is not a morph curve key");
    Check(slot == "org.mikudancestudio.model.3.morphCurve.v1", "PMM key layout is pinned");

    mikumikustudio::pmm_extension::Document document;
    mc::CapturePmm(app, document);
    const auto* firstRecord = mikumikustudio::pmm_extension::Find(document, mc::PmmKey(0));
    const auto* secondRecord = mikumikustudio::pmm_extension::Find(document, mc::PmmKey(1));
    Check(firstRecord != nullptr && secondRecord != nullptr, "both slots captured");
    // Copy before the document grows: appending a record may reallocate it.
    const std::vector<std::uint8_t> firstValue =
        firstRecord != nullptr ? firstRecord->value : std::vector<std::uint8_t>{};

    // A record of an unknown version must survive a capture verbatim while the
    // model it describes still matches.
    mikumikustudio::pmm_extension::Set(
        document, "org.mikudancestudio.model.0.morphCurve.v9", {1, 2, 3});

    mc::Reset(&app);
    Check(!mc::AnyCurve(first.raw()) && !mc::AnyCurve(second.raw()), "Reset drops every curve");

    mc::LoadPmm(app, document);
    Check(mc::AnyCurve(first.raw()) && mc::AnyCurve(second.raw()), "PMM load installs both slots");
    mc::Evaluate(first.raw(), 12.0);
    mc::Evaluate(second.raw(), 6.0);
    Check(Near(first.value(0), 0.5, 1e-6), "slot 0 curve restored");
    Check(Near(second.value(0), 0.375, 1e-6), "slot 1 curve restored");

    // A record whose model no longer matches must be dropped, while a matching
    // one is kept verbatim, and an unknown version survives next to it.
    mc::Reset(&app);
    second.morphs[0].name[0] = 'Z';
    second.storage->morphs = second.morphs.data();
    mc::CapturePmm(app, document);
    Check(mikumikustudio::pmm_extension::Find(document, mc::PmmKey(0)) != nullptr,
          "a matching record keeps its bytes");
    Check(mikumikustudio::pmm_extension::Find(document, mc::PmmKey(1)) == nullptr,
          "a record for a different model is dropped");
    Check(mikumikustudio::pmm_extension::Find(
              document, "org.mikudancestudio.model.0.morphCurve.v9") != nullptr,
          "an unknown version of a matching slot is preserved");
    Check(mikumikustudio::pmm_extension::Find(document, mc::PmmKey(0))->value == firstValue,
          "the kept record is byte-identical");

    // A record that does not describe the loaded model is ignored and keeps its
    // raw bytes.
    mikumikustudio::pmm_extension::Document broken;
    mikumikustudio::pmm_extension::Set(broken, mc::PmmKey(0), {9, 9, 9});
    mc::LoadPmm(app, broken);
    Check(!mc::AnyCurve(first.raw()), "a malformed record installs nothing");
    Check(mikumikustudio::pmm_extension::Find(broken, mc::PmmKey(0))->value.size() == 3,
          "a malformed record keeps its bytes");

    app.ModelSlot(0) = nullptr;
    app.ModelSlot(1) = nullptr;
}

// ---------------------------------------------------------------------------
// VMD roundtrip, standard prefix readability and bone-scale coexistence
// ---------------------------------------------------------------------------

// Calls the port's own standard-section reader on a buffer that may carry the
// morph curve block behind the standard body.
void curve_motion_check(const std::vector<std::uint8_t>& bytes, bool expectTrack);

void VmdRoundTripAndCoexistence() {
    FakeModel source = MakeModel({{"eye", 2}});
    NativeTrack(source, 0, {{0, 0.0f}});
    std::string error;
    Check(mc::Write(source.raw(), {Named(source, 0, Curve({{0, 0.0}, {20, 1.0}}))}, error),
          "source authored");

    std::vector<std::uint8_t> block;
    Check(mc::CaptureVmd(source.raw(), 0, block, error), "the VMD block is built");
    Check(!block.empty() && mc::VmdBlockSize(block.data(), block.size()) == block.size(),
          "the block is self-delimiting");
    std::vector<std::uint8_t> payload;
    Check(mc::DecodeVmdBlock(block.data(), block.size(), payload) == block.size(),
          "the block decodes");

    // Roundtrip into the same model names, at an import offset.  The target is
    // given the native keys the standard section would have registered, so the
    // block describes a real import.
    FakeModel target = MakeModel({{"eye", 2}});
    NativeTrack(target, 0, {{0, 0.0f}});
    NativeAdd(target, 0, 120, 0.0f);
    NativeAdd(target, 0, 140, 1.0f);
    Check(mc::ApplyVmd(target.raw(), block.data(), block.size(), 120) == 1,
          "the block applies to a name-matched model");
    mc::Evaluate(target.raw(), 130.0);
    Check(Near(target.value(0), 0.5, 1e-6), "the imported curve is shifted by the import offset");
    mc::Evaluate(target.raw(), 120.0);
    Check(Near(target.value(0), 0.0, 1e-6), "the shifted curve keeps its first key");

    // The same block against a model that did not receive those keys must be
    // refused as a whole rather than rewriting frames it did not come with.
    FakeModel unimported = MakeModel({{"eye", 2}});
    NativeTrack(unimported, 0, {{0, 0.0f}});
    Check(mc::ApplyVmd(unimported.raw(), block.data(), block.size(), 120) == 0,
          "a block whose frames were not imported is refused");
    Check(!mc::AnyCurve(unimported.raw()), "the refused block installs nothing");

    // A model without that morph name is left alone.
    FakeModel foreign = MakeModel({{"noteye", 2}});
    NativeTrack(foreign, 0, {{0, 0.0f}});
    Check(mc::ApplyVmd(foreign.raw(), block.data(), block.size(), 0) == 0,
          "an unmatched name installs nothing");
    Check(!mc::AnyCurve(foreign.raw()), "the unmatched model gains no authoring");

    // A damaged body must be skippable but must not install anything.
    std::vector<std::uint8_t> damaged = block;
    damaged[damaged.size() - 1] ^= 0x01;
    Check(mc::VmdBlockSize(damaged.data(), damaged.size()) == damaged.size(),
          "a damaged body is still skipped by its declared size");
    std::vector<std::uint8_t> scratch;
    Check(mc::DecodeVmdBlock(damaged.data(), damaged.size(), scratch) == 0,
          "a bad checksum refuses the block");
    FakeModel damagedTarget = MakeModel({{"eye", 2}});
    NativeTrack(damagedTarget, 0, {{0, 0.0f}});
    Check(mc::ApplyVmd(damagedTarget.raw(), damaged.data(), damaged.size(), 0) == 0,
          "a damaged block applies nothing");

    // The standard prefix stays a standard VMD the existing standard-section
    // reader can import, with and without the block behind it.
    const std::vector<std::uint8_t> standard =
        StandardVmd({{"eye", 0, 0.0f}, {"eye", 20, 1.0f}});
    curve_motion_check(standard, true);
    std::vector<std::uint8_t> whole = standard;
    whole.insert(whole.end(), block.begin(), block.end());
    curve_motion_check(whole, true);

    // The loader protocol itself: the probe reads sixteen bytes and must be told
    // the whole declared size from them, and the body must then decode from the
    // whole block.  A previous contract that refused a declared size larger than
    // the buffer made every real load skip the block silently.
    Check(mc::VmdBlockSize(block.data(), 16) == block.size(),
          "a header-only probe reports the declared block size");
    std::vector<std::uint8_t> headerOnly;
    Check(mc::DecodeVmdBlock(block.data(), 16, headerOnly) == 0,
          "a block cannot be decoded from its header alone");
    std::vector<std::uint8_t> probed;
    Check(ProbeBlockLikeLoader(whole, standard.size(), probed) && probed == block,
          "the loader protocol finds the block behind the standard body");
    Check(!ProbeBlockLikeLoader(standard, standard.size(), probed),
          "a standard body without a block probes as nothing");

    // The bone-scale container must still validate with the block in front of
    // it: its recorded prefix covers the block and its footer stays last.
    {
        TempMotion file;
        int fd = file.Open(_O_WRONLY | _O_TRUNC);
        Check(fd >= 0, "temporary motion opened");
        Check(_write(fd, whole.data(), static_cast<unsigned>(whole.size())) ==
                  static_cast<int>(whole.size()),
              "temporary motion written");
        bsio::PrefixFingerprint fingerprint;
        fingerprint.Append(whole.data(), whole.size());
        bsio::VmdExtension extension;
        extension.prefixBytes = fingerprint.bytes();
        extension.prefixCrc32 = fingerprint.crc32();
        bsio::VmdExtension::Entry entry;
        const char bone[] = "center";
        std::memcpy(entry.name.data(), bone, sizeof(bone) - 1);
        entry.frame = 7;
        entry.scale.v = {2.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        extension.entries.push_back(entry);
        Check(bsio::SaveVmdExtension(fd, extension) == bsio::ExtensionSave::Written,
              "the bone-scale tail is written behind the block");
        _close(fd);

        const std::uint64_t boundary = static_cast<std::uint64_t>(whole.size());
        bsio::VmdExtension loaded;
        Check(bsio::LoadVmdExtension(file.path(), loaded, static_cast<std::int64_t>(boundary)) ==
                  bsio::ExtensionLoad::Loaded &&
                  loaded.entries.size() == 1,
              "the container validates with the morph curve block in its prefix");
        Check(loaded.prefixBytes == boundary,
              "the container prefix is the standard body plus the block");

        const std::vector<std::uint8_t> onDisk = file.Read();
        Check(onDisk.size() > boundary && mc::VmdBlockSize(onDisk.data() + standard.size(),
                                                          onDisk.size() - standard.size()) ==
                                              block.size(),
              "the block is still readable in the saved file");
        Check(bsio::LoadVmdExtension(file.path(), loaded,
                                     static_cast<std::int64_t>(standard.size())) ==
                  bsio::ExtensionLoad::Ignored,
              "a boundary that ignores the block is refused by the container");
    }
}

// ---------------------------------------------------------------------------
// VMD export selection and import merge boundaries
// ---------------------------------------------------------------------------

void VmdSelectionAndImportMerge() {
    // ---- export: exactly the marked keys, at the exporter's own minimum ------
    FakeModel source = MakeModel({{"eye", 2}});
    NativeTrack(source, 0, {{0, 0.0f}});
    ac::Track four = Curve({{0, 0.0}, {10, 0.3}, {20, 0.6}, {30, 1.0}});
    EaseInto(four, 1, 0.1);  // the [0, 10] interval is eased
    EaseInto(four, 3, 0.1);  // and the [20, 30] interval
    std::string error;
    Check(mc::Write(source.raw(), {Named(source, 0, four)}, error), "four-key curve installed");

    // (a) only a non-zero sub-range is marked: the curve must survive rebased,
    //     instead of being dropped because its first key is below minFrame.
    NativeClearMarks(source, 0);
    Check(NativeSetMark(source, 0, 20, 1) && NativeSetMark(source, 0, 30, 1), "mark 20 and 30");
    std::vector<std::uint8_t> block;
    Check(mc::CaptureVmd(source.raw(), 20, block, error) && !block.empty(),
          "a marked sub-range still exports");
    std::vector<ac::NamedTrack> tracks = BlockTracks(block);
    Check(tracks.size() == 1 && tracks[0].track.keys.size() == 2,
          "only the two marked keys are exported");
    Check(tracks[0].track.keys[0].frame == 0 && Near(tracks[0].track.keys[0].value, 0.6) &&
              tracks[0].track.keys[1].frame == 10 && Near(tracks[0].track.keys[1].value, 1.0),
          "the marked sub-range is rebased by the exporter's minimum frame");
    Check(tracks[0].track.keys[1].interpolation == ac::Interpolation::Bezier,
          "an authored interval whose two keys were both exported keeps its ease");

    // (b) an unmarked key in the middle is left out, it is not invented again,
    //     and the interval that lost its neighbour falls back to linear.
    NativeClearMarks(source, 0);
    Check(NativeSetMark(source, 0, 0, 1) && NativeSetMark(source, 0, 10, 1) &&
              NativeSetMark(source, 0, 30, 1),
          "mark 0, 10 and 30");
    Check(mc::CaptureVmd(source.raw(), 0, block, error), "the marked keys export");
    tracks = BlockTracks(block);
    Check(tracks.size() == 1 && tracks[0].track.keys.size() == 3, "the unmarked key is skipped");
    Check(tracks[0].track.keys[0].frame == 0 && tracks[0].track.keys[1].frame == 10 &&
              tracks[0].track.keys[2].frame == 30,
          "no key is invented for the skipped frame");
    Check(tracks[0].track.keys[1].interpolation == ac::Interpolation::Bezier,
          "an interval with both of its keys exported keeps its ease");
    Check(tracks[0].track.keys[2].interpolation == ac::Interpolation::Linear,
          "an interval whose neighbour was skipped loses its ease");

    // (c) nothing marked: nothing is written and the caller adds no block.
    NativeClearMarks(source, 0);
    Check(mc::CaptureVmd(source.raw(), 0, block, error) && block.empty(),
          "a curve with no marked key exports nothing");

    // ---- import: merge into the target, keep what was not imported ----------
    // The target already owns a curve with a frame outside the imported range
    // and one inside it, and the import registers the file's keys at an offset,
    // exactly as vmd_load does.
    FakeModel target = MakeModel({{"eye", 2}});
    NativeTrack(target, 0, {{0, 0.4f}});
    ac::Track existing = Curve({{0, 0.4}, {60, 0.9}, {115, 0.5}});
    EaseInto(existing, 1, 0.1);
    EaseInto(existing, 2, 0.1);
    Check(mc::Write(target.raw(), {Named(target, 0, existing)}, error), "target curve installed");
    NativeAdd(target, 0, 140, 0.7f);
    NativeAdd(target, 0, 150, 0.8f);
    ac::Track file = Curve({{40, 0.7}, {50, 0.8}});
    EaseInto(file, 1, 0.1);
    const std::vector<std::uint8_t> importBlock = BlockOf({Named(target, 0, file)});
    Check(!importBlock.empty(), "import block built");
    Check(mc::ApplyVmd(target.raw(), importBlock.data(), importBlock.size(), 100) == 1,
          "the matching block applies to the target");

    ac::Track read;
    Check(mc::Read(target.raw(), 0, read, error) && read.keys.size() == 5,
          "the import merges instead of replacing the whole curve");
    Check(read.keys[0].frame == 0 && Near(read.keys[0].value, 0.4) &&
              read.keys[1].frame == 60 && Near(read.keys[1].value, 0.9) &&
              read.keys[2].frame == 115 && Near(read.keys[2].value, 0.5),
          "the frames outside the imported range are untouched");
    Check(read.keys[1].interpolation == ac::Interpolation::Bezier &&
              read.keys[2].interpolation == ac::Interpolation::Bezier,
          "their authored ease is untouched too");
    Check(read.keys[3].frame == 140 && Near(read.keys[3].value, 0.7) &&
              read.keys[4].frame == 150 && Near(read.keys[4].value, 0.8),
          "the imported keys land on the offset frames");
    Check(read.keys[4].interpolation == ac::Interpolation::Bezier,
          "an imported interval whose keys stayed neighbours keeps its ease");
    Check(!Near(read.keys[0].value, 0.7),
          "frame zero keeps the target's own value, not the imported first key");
    const double expectedAt145 = ac::Evaluate(read, 145.0).value;
    Check(std::fabs(expectedAt145 - 0.75) > 0.01,
          "the imported interval really is eased, not a linear blend");
    mc::Evaluate(target.raw(), 145.0);
    Check(Near(target.value(0), expectedAt145, 1e-6), "the imported interval plays its own shape");

    // A target key sitting between two imported keys survives, and neither the
    // target's interval nor the imported one may keep an ease over the new span.
    FakeModel split = MakeModel({{"eye", 2}});
    NativeTrack(split, 0, {{0, 0.4f}});
    Check(mc::Write(split.raw(), {Named(split, 0, existing)}, error), "split target installed");
    NativeAdd(split, 0, 110, 0.1f);
    NativeAdd(split, 0, 120, 0.2f);
    ac::Track splitFile = Curve({{10, 0.1}, {20, 0.2}});
    EaseInto(splitFile, 1, 0.1);
    const std::vector<std::uint8_t> splitBlock = BlockOf({Named(split, 0, splitFile)});
    Check(mc::ApplyVmd(split.raw(), splitBlock.data(), splitBlock.size(), 100) == 1,
          "the splitting import applies");
    Check(mc::Read(split.raw(), 0, read, error) && read.keys.size() == 5,
          "the unnamed target key inside the range is kept");
    Check(read.keys[3].frame == 115 && Near(read.keys[3].value, 0.5),
          "its value survives the import");
    Check(read.keys[3].interpolation == ac::Interpolation::Linear &&
              read.keys[4].interpolation == ac::Interpolation::Linear,
          "no ease is claimed over a span the import does not own");
    Check(read.keys[1].interpolation == ac::Interpolation::Bezier,
          "a target interval the import did not touch keeps its ease");

    // A block whose values do not match the import is refused, and an ambiguous
    // name is skipped instead of guessed.
    ac::Track wrongValues = Curve({{40, 0.25}, {50, 0.8}});
    const std::vector<std::uint8_t> wrongBlock = BlockOf({Named(target, 0, wrongValues)});
    FakeModel mismatch = MakeModel({{"eye", 2}});
    NativeTrack(mismatch, 0, {{0, 0.0f}});
    NativeAdd(mismatch, 0, 140, 0.7f);
    NativeAdd(mismatch, 0, 150, 0.8f);
    Check(mc::ApplyVmd(mismatch.raw(), wrongBlock.data(), wrongBlock.size(), 100) == 0,
          "a block whose value does not match the import is refused");
    Check(!mc::AnyCurve(mismatch.raw()), "the refused value mismatch installs nothing");

    FakeModel ambiguous = MakeModel({{"eye", 2}, {"eye", 3}});
    NativeTrack(ambiguous, 0, {{0, 0.4f}});
    NativeTrack(ambiguous, 1, {{0, 0.4f}});
    NativeAdd(ambiguous, 0, 140, 0.7f);
    NativeAdd(ambiguous, 1, 140, 0.7f);
    Check(mc::ApplyVmd(ambiguous.raw(), importBlock.data(), importBlock.size(), 100) == 0,
          "an ambiguous morph name is skipped");
    Check(!mc::AnyCurve(ambiguous.raw()), "the ambiguous name installs nothing");
}

// Calls the port's own standard-section reader on a buffer that may carry the
// morph curve block behind the standard body.
void curve_motion_check(const std::vector<std::uint8_t>& bytes, bool expectTrack) {
    mikumikustudio::curve_motion::Motion motion;
    std::string error;
    const bool decoded = mikumikustudio::curve_motion::Decode(
        bytes.data(), bytes.size(), mikumikustudio::curve_motion::Kind::Morph, motion, error);
    Check(decoded, "a standard-section reader still imports the prefix");
    if (!decoded) return;
    const bool found = !motion.tracks.empty();
    Check(found == expectTrack, "the prefix morph records reach the reader");
    if (found) {
        Check(motion.tracks[0].id == "eye" && motion.tracks[0].track.keys.size() == 2,
              "the prefix morph proxy is the linear one");
    }
}

// ---------------------------------------------------------------------------
// Codec rejections
// ---------------------------------------------------------------------------

void CodecRejects() {
    std::vector<std::uint8_t> payload;
    std::string error;
    const ac::NamedTrack named{"m0:" + std::string(40, '0'), Curve({{0, 0.0}, {5, 1.0}})};
    Check(mc::EncodePayload({named}, payload, error) && !payload.empty(), "payload encodes");
    std::vector<ac::NamedTrack> decoded;
    Check(mc::DecodePayload(payload.data(), payload.size(), decoded, error) &&
              decoded.size() == 1 && decoded[0].track.keys.size() == 2,
          "payload decodes");
    Check(!mc::DecodePayload(payload.data(), payload.size() - 1, decoded, error),
          "a truncated payload is refused");
    std::vector<std::uint8_t> wrong = payload;
    wrong[0] = 'X';
    Check(!mc::DecodePayload(wrong.data(), wrong.size(), decoded, error),
          "a wrong payload magic is refused");
    wrong = payload;
    wrong[5] = 2;  // version high byte
    Check(!mc::DecodePayload(wrong.data(), wrong.size(), decoded, error),
          "an unknown payload version is refused");
    Check(!mc::EncodeVmdBlock({}, payload, error), "an empty block is refused");
    Check(mc::VmdBlockSize(payload.data(), payload.size()) == 0, "a payload is not a block");
    Check(mc::VmdBlockSize(payload.data(), 8) == 0,
          "a buffer shorter than a block header is not a block");

    int index = -1;
    std::string name;
    Check(!mc::ParseStableId("nonsense", index, name), "a foreign id is refused");
    Check(!mc::ParseStableId("m1:zz", index, name), "a non-hex id is refused");
    Check(!mc::ParseStableId(mc::StableId(1, nullptr), index, name),
          "an id built without name bytes is refused");
}


void DenseWriteAndGrowthIdentity() {
    auto model = MakeModel({{"smile", 1}, {"blink", 2}});
    auto& record = *model.record();
    record.morphKeys = mdl::MorphKeyPoolCreate(mdl::kMorphKeyCapacity);
    std::string error;
    const auto authored = EasedCurve();
    Check(mc::Write(model.raw(), {Named(model, 1, authored)}, error),
          "install an authored curve before growth");
    const auto identity = mdl::MorphKeyPoolIdentity(record.morphKeys);
    auto app = std::make_unique<mikumikustudio::MMDApp>();
    app->ModelSlot(0) = model.raw(); app->SetSelectedModelSlot(0);
    const auto editorIdentity = mikumikustudio::curve_host::MorphIdentityOf(*app, model.raw());
    ac::Track dense;
    for (int frame = 0; frame <= 22000; ++frame) {
        ac::Key key; key.frame = frame; key.value = frame / 22000.0;
        key.interpolation = ac::Interpolation::Linear; key.tangent = ac::Tangent::Linear;
        dense.keys.push_back(key);
    }
    Check(mc::Write(model.raw(), {Named(model, 0, dense)}, error),
          "curve write grows beyond 20000 native slots");
    Check(mdl::MorphKeyCapacity(record) > mdl::kMorphKeyCapacity &&
              mdl::MorphKeyPoolIdentity(record.morphKeys) == identity,
          "morph pool identity survives growth");
    Check(mikumikustudio::curve_host::MorphIdentityCurrent(*app, editorIdentity),
          "the curve editor remains bound to the same model after growth");
    ac::Track first, second;
    Check(mc::Read(model.raw(), 0, first, error) && mc::Read(model.raw(), 1, second, error),
          "grown curves can be read");
    Check(first.keys.size() == 22001, "all dense curve keys are retained");
    Check(second.keys.size() == 2 && second.keys[1].inSlope == authored.keys[1].inSlope,
          "growth preserves another morph's authored tangents");
    std::vector<std::uint8_t> block;
    Check(mc::CaptureVmd(model.raw(), 0, block, error), "grown curves serialize to VMD extension");
    const auto decoded = BlockTracks(block);
    Check(decoded.size() == 2 && decoded[0].track.keys.size() == 22001,
          "VMD extension retains every grown key");
    mc::Forget(model.raw());
    mdl::MorphKeyPoolDestroy(record.morphKeys);
}

}  // namespace

int main() {
    DenseWriteAndGrowthIdentity();
    EnumerationAndIds();
    NativeReadAndIdentity();
    WriteSamplingAndRejection();
    NativeEditsInvalidate();
    NativeMarksAreNotOccupancy();
    AuthoredFallbackAndRestore();
    ModelIsolationAndIdentity();
    PmmRoundTrip();
    VmdRoundTripAndCoexistence();
    VmdSelectionAndImportMerge();
    CodecRejects();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %d checks (native morph curves)\n", g_checks);
    return 0;
}
