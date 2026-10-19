#include <algorithm>
#include "mikumikustudio/extended_names.hpp"
// Persistence for native bone scaling: the PMM v2
// "org.mikudancestudio.model.<slot>.boneScale.v1" (or ".v2") tail record and
// the VMD tail extension ("MMSXVMDX" footer after the standard prefix; legacy
// "MDSXVMDX" is also read).  Wire
// layouts and strictness rules are documented in
// include/mikumikustudio/bone_scale_io.hpp.
//
// The codec part is deterministic CPU code with its own matrix validation,
// byte readers and CRC32, so a test can pin the wire format without a model.
// The model-facing part at the bottom reads and writes bone scale only
// through the bone_scale.hpp API; no legacy bone or key structure is touched
// and no key-slot number is ever persisted.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/runtime_log.hpp"

namespace mikumikustudio {
namespace bone_scale_io {
namespace {

// ---- CRC32 (IEEE, reflected, polynomial 0xEDB88320) ----------------------

struct Crc32Table {
    std::uint32_t value[256];
    Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            value[i] = crc;
        }
    }
};

std::uint32_t Crc32Update(std::uint32_t crc, const void* data, std::size_t size) {
    static const Crc32Table table;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ table.value[(crc ^ bytes[i]) & 0xFFu];
    return crc;
}

std::uint32_t Crc32Of(const std::uint8_t* data, std::size_t size) {
    return Crc32Update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

// ---- little-endian byte writers and readers ------------------------------

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    PutU32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
    PutU32(out, static_cast<std::uint32_t>(value >> 32));
}

void PutF32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    PutU32(out, bits);
}

struct Reader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    bool U32(std::uint32_t& out) {
        if (size - pos < 4) return false;
        out = static_cast<std::uint32_t>(data[pos]) |
              (static_cast<std::uint32_t>(data[pos + 1]) << 8) |
              (static_cast<std::uint32_t>(data[pos + 2]) << 16) |
              (static_cast<std::uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    }
    bool U64(std::uint64_t& out) {
        std::uint32_t low = 0, high = 0;
        if (!U32(low) || !U32(high)) return false;
        out = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
        return true;
    }
    bool I32(std::int32_t& out) {
        std::uint32_t bits = 0;
        if (!U32(bits)) return false;
        std::memcpy(&out, &bits, sizeof out);
        return true;
    }
    bool F32(float& out) {
        std::uint32_t bits = 0;
        if (!U32(bits)) return false;
        std::memcpy(&out, &bits, sizeof out);
        return true;
    }
    bool Bytes(void* out, std::size_t count) {
        if (size - pos < count) return false;
        std::memcpy(out, data + pos, count);
        pos += count;
        return true;
    }
};

// ---- matrix validation (mirrors bone_scale.cpp semantics) ----------------

double Determinant(const bone_scale::Matrix& a) {
    const auto& m = a.v;
    return double(m[0]) * (double(m[4]) * m[8] - double(m[5]) * m[7]) -
           double(m[1]) * (double(m[3]) * m[8] - double(m[5]) * m[6]) +
           double(m[2]) * (double(m[3]) * m[7] - double(m[4]) * m[6]);
}

bool InverseMatrix(const bone_scale::Matrix& a, bone_scale::Matrix& out) {
    const auto& m = a.v;
    const double d = Determinant(a);
    if (!std::isfinite(d) || std::abs(d) < 1.e-18) return false;
    const double cof[9] = {
        double(m[4]) * m[8] - double(m[5]) * m[7],
        double(m[2]) * m[7] - double(m[1]) * m[8],
        double(m[1]) * m[5] - double(m[2]) * m[4],
        double(m[5]) * m[6] - double(m[3]) * m[8],
        double(m[0]) * m[8] - double(m[2]) * m[6],
        double(m[2]) * m[3] - double(m[0]) * m[5],
        double(m[3]) * m[7] - double(m[4]) * m[6],
        double(m[1]) * m[6] - double(m[0]) * m[7],
        double(m[0]) * m[4] - double(m[1]) * m[3]};
    for (int i = 0; i < 9; ++i) out.v[i] = float(cof[i] / d);
    return true;
}

bool ValidMatrix(const bone_scale::Matrix& a) {
    for (float v : a.v)
        if (!std::isfinite(v) || std::abs(v) > 10000.f) return false;
    if (Determinant(a) <= 1.e-12) return false;
    bone_scale::Matrix inv;
    if (!InverseMatrix(a, inv)) return false;
    for (float v : inv.v)
        if (!std::isfinite(v) || std::abs(v) > 10000.f) return false;
    return true;
}

bool IdentityMatrix(const bone_scale::Matrix& a) {
    return a.v == bone_scale::Matrix{}.v;
}

// The matrix and the curve are two independent halves of one key slot: the
// curve of a key still shapes the interval that ends at it when the scale has
// gone back to 1, and the model stores both halves separately
// (bone_scale::SetKey drops only the matrix of an identity value, and
// bone_scale::SetKeyCurve stores a linear curve as "no entry").  A key is
// therefore worth a wire record when it holds either a real matrix or a
// non-linear curve; a key that holds neither is the default state and stays out
// of the payload, which is what keeps an untimed capture byte-for-byte the
// version 1 payload.
bool WritesKey(const bone_scale::Matrix& scale, const bone_scale::Curve& curve) {
    if (!ValidMatrix(scale)) return false;
    return !IdentityMatrix(scale) || !bone_scale::Linear(curve);
}

// ---- SJIS name comparison ------------------------------------------------

// strcmp semantics inside two NUL-padded fixed fields.
bool FieldEquals(const char* a, std::size_t aSize, const char* b, std::size_t bSize) {
    for (std::size_t i = 0;; ++i) {
        const char ca = i < aSize ? a[i] : '\0';
        const char cb = i < bSize ? b[i] : '\0';
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

// The VMD loader truncates the 15-byte name field at the first LF before it
// matches bones; do the same here so a extension entry can never land on a key
// the import did not create.
bool VmdNameEquals(const std::array<char, 15>& name, const char* boneName,
                   std::size_t boneNameSize) {
    char truncated[16];
    std::memcpy(truncated, name.data(), 15);
    truncated[15] = '\0';
    if (char* newline = std::strchr(truncated, '\n')) *newline = '\0';
    return FieldEquals(truncated, sizeof truncated, boneName, boneNameSize);
}

// ---- size limits ---------------------------------------------------------

// Decode-side corruption guards.  The encoders never truncate (see the
// header): a model may legitimately hold more samples than a reader would
// accept from an untrusted file, and silently dropping scale is exactly what
// a save must never do.
constexpr std::uint32_t kMaxPmmPose = 1u << 20;
constexpr std::uint32_t kMaxPmmKeys = 1u << 20;
constexpr std::uint32_t kMaxExtensionEntries = 1u << 20;
constexpr std::uint64_t kMaxExtensionBytes = PTRDIFF_MAX - 64;
constexpr std::size_t kPmmPoseBytes = 60;
constexpr std::size_t kPmmKeyBytes = 64;
constexpr std::size_t kPmmCurveBytes = 4;      // v2: appended per key record
constexpr std::size_t kExtensionEntryBytes = 55;
constexpr std::size_t kExtensionEntryBytesV2 = kExtensionEntryBytes + 4;
constexpr std::size_t kPayloadHeaderBytes = 32;
constexpr std::size_t kFooterBytes = 16;

// The highest control-point value the curve domain allows (0..127).
constexpr std::uint8_t kMaxCurveByte = 127;

// A curve travels on disk as four bytes; every component is validated against
// the same 0..127 domain the interpolation panel writes.
bool ValidCurveBytes(const std::uint8_t* bytes) {
    for (std::size_t i = 0; i < 4; ++i)
        if (bytes[i] > kMaxCurveByte) return false;
    return true;
}

// ---- VMD tail extension helpers -----------------------------------------

// Payload magic.  New writes use MMS; the MDS form is read for old files.
const char kPayloadMagic[8] = {'M', 'M', 'S', 'X', 'M', 'M', 'S', 'X'};
const char kLegacyPayloadMagic[8] = {'M', 'D', 'S', 'X', 'M', 'D', 'S', 'X'};
// Footer magic: uint64 payloadBytes followed by these 8 bytes at the very end
// of the file locate the extension.
const char kFooterMagic[8] = {'M', 'M', 'S', 'X', 'V', 'M', 'D', 'X'};
const char kLegacyFooterMagic[8] = {'M', 'D', 'S', 'X', 'V', 'M', 'D', 'X'};

// ---- model helpers -------------------------------------------------------

// Owner bone of every key slot: one forward pass over each bone's linked
// track assigns the whole track to that bone, so a full 600000-slot pool is
// resolved once instead of walking backwards per key.  A slot that belongs
// to no track is reported as -1 and skipped; it never falls back onto
// another bone's name.
class KeyOwners {
public:
    // `capacity` is the pool's runtime capacity; the owner table covers it so a
    // key in a slot the pool grew into is still resolved to its bone.
    KeyOwners(const mdl::BoneKey* keys, int boneCount, std::size_t capacity)
        : capacity_(capacity), owner_(capacity, kNone) {
        if (keys == nullptr || boneCount <= 0) return;
        for (int bone = 0; bone < boneCount; ++bone) {
            unsigned int cur = static_cast<unsigned int>(bone);
            unsigned int hops = 0;
            for (;;) {
                if (cur >= capacity_ ||
                    hops++ >= capacity_)
                    break;
                if (owner_[cur] == kNone) owner_[cur] = bone;
                const unsigned int next = keys[cur].next;
                if (next == 0) break;
                cur = next;
            }
        }
    }
    int Get(unsigned int slot) const {
        if (slot >= capacity_)
            return -1;
        const int bone = owner_[slot];
        return bone == kNone ? -1 : bone;
    }

private:
    static constexpr int kNone = -1;
    std::size_t capacity_;
    std::vector<int> owner_;
};

// One pass over every bone's linked track: (bone, frame) -> key slot, with
// -1 marking a frame that appears twice in one track (never guess).
using TrackKey = std::pair<int, std::uint32_t>;
std::map<TrackKey, int> BuildTrackIndex(const mdl::BoneKey* keys,
                                        int boneCount,
                                        std::size_t capacity) {
    std::map<TrackKey, int> index;
    if (keys == nullptr || boneCount <= 0) return index;
    for (int bone = 0; bone < boneCount; ++bone) {
        unsigned int cur = static_cast<unsigned int>(bone);
        unsigned int hops = 0;
        for (;;) {
            if (cur >= capacity ||
                hops++ >= capacity)
                break;
            const TrackKey key{bone, keys[cur].frame};
            const auto it = index.find(key);
            if (it == index.end())
                index.emplace(key, static_cast<int>(cur));
            else
                it->second = -1;
            const unsigned int next = keys[cur].next;
            if (next == 0) break;
            cur = next;
        }
    }
    return index;
}

// The NUL-truncated C string inside a fixed SJIS field.
std::string FieldName(const char* field, std::size_t size) {
    std::size_t length = 0;
    while (length < size && field[length] != '\0') ++length;
    return std::string(field, length);
}

// The VMD loader truncates the 15-byte name field at the first LF before it
// matches bones; do the same here so a extension entry can never land on a key
// the import did not create.
std::string VmdName(const std::array<char, 15>& name) {
    std::size_t length = 0;
    while (length < name.size() && name[length] != '\0' &&
           name[length] != '\n')
        ++length;
    return std::string(name.data(), length);
}

// Snapshot of the model's non-identity pose and linked-track key scales.
Samples GatherSamples(const unsigned char* model) {
    Samples out;
    if (model == nullptr) return out;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const int boneCount = static_cast<int>(record.boneCount);
    const mdl::BoneRecord* bones = record.boneTable;
    const mdl::BoneKey* keys = record.boneKeys;
    if (boneCount <= 0 || bones == nullptr || keys == nullptr) return out;
    // Runtime pool capacity, resolved once for both sweeps below.
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(record);
    const bone_scale::Snapshot snapshot = bone_scale::Capture(model);
    for (const auto& entry : snapshot.pose) {
        const int bone = entry.first;
        if (bone < 0 || bone >= boneCount) continue;
        if (IdentityMatrix(entry.second) || !ValidMatrix(entry.second)) continue;
        PoseSample sample;
        sample.bone = bone;
        std::memcpy(sample.name.data(), bones[bone].name, sample.name.size());
        sample.scale = entry.second;
        out.pose.push_back(sample);
    }
    for (int bone = 0; bone < boneCount; ++bone) {
        unsigned int cur = static_cast<unsigned int>(bone);
        unsigned int hops = 0;
        for (;;) {
            if (cur >= boneKeyCapacity ||
                hops++ >= boneKeyCapacity)
                break;
            // Both halves of the slot are read together.  A slot can hold a
            // curve with no matrix - the scale went back to 1 and the curve
            // still describes the interval that ends at that key - so the
            // matrix lookup alone would lose it.
            const auto matrix = snapshot.keys.find(static_cast<int>(cur));
            const auto curve = snapshot.curves.find(static_cast<int>(cur));
            if (matrix != snapshot.keys.end() || curve != snapshot.curves.end()) {
                const bone_scale::Matrix scale = matrix == snapshot.keys.end()
                                                     ? bone_scale::Matrix{}
                                                     : matrix->second;
                const bone_scale::Curve value = curve == snapshot.curves.end()
                                                    ? bone_scale::LinearCurve()
                                                    : curve->second;
                if (WritesKey(scale, value)) {
                    KeySample sample;
                    sample.bone = bone;
                    std::memcpy(sample.name.data(), bones[bone].name,
                                sample.name.size());
                    sample.frame = keys[cur].frame;
                    sample.scale = scale;
                    sample.curve = value;
                    out.keys.push_back(sample);
                }
            }
            const unsigned int next = keys[cur].next;
            if (next == 0) break;
            cur = next;
        }
    }
    return out;
}

// Validates a decoded block against the model and resolves every key sample
// to its current key slot.  Bone identity (index + name), indices, duplicate
// keys and track membership are all checked before anything is applied, so a
// block that cannot be applied in full leaves the model untouched.
bool BuildSnapshot(const unsigned char* model, const Samples& samples,
                   bone_scale::Snapshot& out) {
    out = {};
    if (model == nullptr) return false;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const int boneCount = static_cast<int>(record.boneCount);
    const mdl::BoneRecord* bones = record.boneTable;
    const mdl::BoneKey* keys = record.boneKeys;
    if (boneCount <= 0 || bones == nullptr || keys == nullptr)
        return samples.pose.empty() && samples.keys.empty();
    std::set<int> poseSeen;
    for (const PoseSample& sample : samples.pose) {
        if (sample.bone < 0 || sample.bone >= boneCount) return false;
        if (!FieldEquals(sample.name.data(), sample.name.size(),
                         bones[sample.bone].name,
                         sizeof bones[sample.bone].name))
            return false;
        if (!ValidMatrix(sample.scale)) return false;
        if (!poseSeen.insert(sample.bone).second) return false;
        if (IdentityMatrix(sample.scale)) continue;
        out.pose[sample.bone] = sample.scale;
    }
    std::set<std::pair<int, std::uint32_t>> keySeen;
    const std::map<TrackKey, int> trackIndex =
        BuildTrackIndex(keys, boneCount, mdl::BoneKeyCapacity(record));
    for (const KeySample& sample : samples.keys) {
        if (sample.bone < 0 || sample.bone >= boneCount) return false;
        if (!FieldEquals(sample.name.data(), sample.name.size(),
                         bones[sample.bone].name,
                         sizeof bones[sample.bone].name))
            return false;
        if (!ValidMatrix(sample.scale)) return false;
        if (!keySeen.insert({sample.bone, sample.frame}).second) return false;
        const auto track = trackIndex.find({sample.bone, sample.frame});
        if (track == trackIndex.end() || track->second < 0) return false;
        const int slot = track->second;
        // Both halves land on the key slot the bone identity resolved to.  A
        // key whose scale returned to 1 carries no matrix but still carries the
        // curve of the interval that ends at that key, so the two are stored
        // independently; a linear curve has no entry in the model, and the
        // missing entry is exactly the linear default.
        if (!IdentityMatrix(sample.scale)) out.keys[slot] = sample.scale;
        if (!bone_scale::Linear(sample.curve)) out.curves[slot] = sample.curve;
    }
    return true;
}

// Per-slot baseline taken when the project was loaded.  It is never advanced
// by a capture: pmm_settings::Capture hands the document to the save, and a
// save that fails must leave every later capture just as willing to write.
// The baseline remembers the model identity (pointer + bone-name fingerprint),
// the bone_scale::Revision and the frames of the scaled keys: a real edit
// moves the revision (SetPose/SetKey/Restore/Undo/Redo) or reshuffles key
// frames (frame edits do not touch the revision), while seeking - which
// rewrites the interpolated pose through Evaluate - does neither.
struct SlotBaseline {
    const unsigned char* model = nullptr;
    std::uint64_t revision = 0;
    std::uint64_t boneFingerprint = 0;
    std::vector<std::pair<int, std::uint32_t>> keyFrames;
    // An unapplied record must survive saving until scale or model edits.
    bool refused = false;
};

std::map<int, SlotBaseline>& Baselines() {
    static std::map<int, SlotBaseline> slots;
    return slots;
}

std::uint64_t BoneFingerprint(const unsigned char* model) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    std::uint64_t hash = 14695981039346656037ull;  // FNV-1a
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    const std::uint32_t count = record.boneCount;
    mix(&count, sizeof count);
    const mdl::BoneRecord* bones = record.boneTable;
    if (bones != nullptr)
        for (std::uint32_t i = 0; i < count; ++i)
            mix(bones[i].name, sizeof bones[i].name);
    return hash;
}

// The frames of the scaled keys, in the stable capture order.  A frame edit
// shows up here even though it never touches bone_scale::Revision.
std::vector<std::pair<int, std::uint32_t>> KeyFrames(const Samples& samples) {
    std::vector<std::pair<int, std::uint32_t>> frames;
    frames.reserve(samples.keys.size());
    for (const KeySample& sample : samples.keys)
        frames.push_back({sample.bone, sample.frame});
    return frames;
}

// The samples both encoders actually write, in the same order and under the
// same rules (a non-negative bone, a valid matrix or a non-linear curve, the
// first record of a duplicate).  Keeping one collection for the payload writer
// and for the version query is what makes the "any curve" answer agree with the
// bytes the writer produces.
std::vector<const PoseSample*> CollectPoses(const Samples& samples) {
    std::vector<const PoseSample*> poses;
    std::set<int> seen;
    for (const PoseSample& sample : samples.pose) {
        if (sample.bone < 0) continue;
        // A pose has no curve, so it is written only for a real matrix.
        if (IdentityMatrix(sample.scale) || !ValidMatrix(sample.scale)) continue;
        if (!seen.insert(sample.bone).second) continue;
        poses.push_back(&sample);
    }
    return poses;
}

std::vector<const KeySample*> CollectKeys(const Samples& samples) {
    std::vector<const KeySample*> keys;
    std::set<std::pair<int, std::uint32_t>> seen;
    for (const KeySample& sample : samples.keys) {
        if (sample.bone < 0) continue;
        if (!WritesKey(sample.scale, sample.curve)) continue;
        if (!seen.insert({sample.bone, sample.frame}).second) continue;
        keys.push_back(&sample);
    }
    return keys;
}

// The slot's bone-scale record and its layout version: the version 2 record
// when the document holds one, otherwise version 1.  This module writes only
// one of the two per slot, so the preference only decides a hand-edited
// document that holds both.  `version` receives kPmmVersionV1/kPmmVersionV2 and
// is left 0 when the slot has no known record; the caller must decode with the
// version the suffix it found promises, never with a guess.
const pmm_extension::Record* FindSlotRecord(
    const pmm_extension::Document& document, int slot, std::uint32_t& version) {
    if (const pmm_extension::Record* newer =
            pmm_extension::Find(document, PmmKey(slot, kPmmVersionV2))) {
        version = kPmmVersionV2;
        return newer;
    }
    if (const pmm_extension::Record* older =
            pmm_extension::Find(document, PmmKey(slot, kPmmVersionV1))) {
        version = kPmmVersionV1;
        return older;
    }
    version = 0;
    return nullptr;
}

}  // namespace

// ---- PMM v2 record codec -------------------------------------------------

std::string PmmKey(int slot) {
    return PmmKey(slot, kPmmVersionV1);
}

std::string PmmKey(int slot, std::uint32_t version) {
    return std::string("org.mikudancestudio.model.") + std::to_string(slot) +
           ".boneScale.v" + std::to_string(version);
}

bool ParsePmmKey(const std::string& key, int& slot, std::string& version) {
    static const std::string kPrefix = "org.mikudancestudio.model.";
    static const std::string kMiddle = ".boneScale.";
    if (key.size() <= kPrefix.size() + kMiddle.size()) return false;
    if (key.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    std::size_t pos = kPrefix.size();
    if (pos >= key.size() || key[pos] < '0' || key[pos] > '9') return false;
    std::uint64_t value = 0;
    while (pos < key.size() && key[pos] >= '0' && key[pos] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(key[pos] - '0');
        if (value > 1000000ull) return false;
        ++pos;
    }
    if (key.compare(pos, kMiddle.size(), kMiddle) != 0) return false;
    version = key.substr(pos + kMiddle.size());
    if (version.empty()) return false;
    slot = static_cast<int>(value);
    return true;
}

std::vector<std::uint8_t> EncodePmm(const Samples& samples) {
    std::vector<std::uint8_t> out;
    const std::vector<const PoseSample*> pose = CollectPoses(samples);
    const std::vector<const KeySample*> keys = CollectKeys(samples);
    bool withCurves = false;
    for (const KeySample* sample : keys)
        if (!bone_scale::Linear(sample->curve)) withCurves = true;
    PutU32(out, static_cast<std::uint32_t>(pose.size()));
    PutU32(out, static_cast<std::uint32_t>(keys.size()));
    for (const PoseSample* sample : pose) {
        PutU32(out, static_cast<std::uint32_t>(sample->bone));
        out.insert(out.end(), sample->name.begin(), sample->name.end());
        for (float v : sample->scale.v) PutF32(out, v);
    }
    for (const KeySample* sample : keys) {
        PutU32(out, static_cast<std::uint32_t>(sample->bone));
        out.insert(out.end(), sample->name.begin(), sample->name.end());
        PutU32(out, sample->frame);
        for (float v : sample->scale.v) PutF32(out, v);
    }
    // Version 2: one 4-byte curve per key record, in the same order.  Without
    // a single curve this loop writes nothing and the payload stays the exact
    // bytes version 1 produced.
    if (withCurves)
        for (const KeySample* sample : keys)
            out.insert(out.end(), sample->curve.begin(), sample->curve.end());
    return out;
}

bool PmmHasCurves(const Samples& samples) {
    for (const KeySample* sample : CollectKeys(samples))
        if (!bone_scale::Linear(sample->curve)) return true;
    return false;
}

namespace {

// One layout decoder.  `version` is the layout the caller's record key names:
// passing it makes the expected curve array exact, so a version 2 payload whose
// curve bytes are missing is short for version 2 and is refused instead of
// being read as a version 1 payload with the curves silently dropped.  A
// `version` of 0 means "the caller has no record key" (the byte-level
// regression), and then the exact size picks the layout.
PmmDecode DecodePmmLayout(const std::uint8_t* data, std::size_t size,
                          std::uint32_t version, Samples& out) {
    out = {};
    if (data == nullptr) return PmmDecode::BadData;
    if (version != 0 && version != kPmmVersionV1 && version != kPmmVersionV2)
        return PmmDecode::BadData;
    Reader reader{data, size, 0};
    std::uint32_t poseCount = 0, keyCount = 0;
    if (!reader.U32(poseCount) || !reader.U32(keyCount))
        return PmmDecode::BadData;
    if (poseCount > kMaxPmmPose || keyCount > kMaxPmmKeys)
        return PmmDecode::BadData;
    // Exact size: rejects truncation, trailing garbage and count overflow.  The
    // two known layouts differ by the version 2 curve array, so the size
    // decides which one a caller without a record key is holding; a payload
    // that fits neither is refused instead of being read with the wrong stride.
    const std::uint64_t version1 =
        8ull + std::uint64_t(poseCount) * kPmmPoseBytes +
        std::uint64_t(keyCount) * kPmmKeyBytes;
    const std::uint64_t version2 =
        version1 + std::uint64_t(keyCount) * kPmmCurveBytes;
    const std::uint64_t actual = static_cast<std::uint64_t>(size);
    bool withCurves = false;
    if (version == kPmmVersionV2) {
        if (actual != version2) return PmmDecode::BadData;
        withCurves = true;
    } else if (version == kPmmVersionV1) {
        if (actual != version1) return PmmDecode::BadData;
    } else {
        if (actual != version1 && actual != version2) return PmmDecode::BadData;
        withCurves = actual == version2;
    }
    out.pose.reserve(poseCount);
    out.keys.reserve(keyCount);
    std::set<int> poseSeen;
    for (std::uint32_t i = 0; i < poseCount; ++i) {
        PoseSample sample;
        if (!reader.I32(sample.bone) ||
            !reader.Bytes(sample.name.data(), sample.name.size()))
            return PmmDecode::BadData;
        bool matrixOk = true;
        for (float& v : sample.scale.v)
            if (!reader.F32(v)) matrixOk = false;
        if (!matrixOk || sample.bone < 0 || !ValidMatrix(sample.scale))
            return PmmDecode::BadData;
        if (!poseSeen.insert(sample.bone).second) return PmmDecode::BadData;
        out.pose.push_back(sample);
    }
    std::set<std::pair<int, std::uint32_t>> keySeen;
    for (std::uint32_t i = 0; i < keyCount; ++i) {
        KeySample sample;
        if (!reader.I32(sample.bone) ||
            !reader.Bytes(sample.name.data(), sample.name.size()) ||
            !reader.U32(sample.frame))
            return PmmDecode::BadData;
        bool matrixOk = true;
        for (float& v : sample.scale.v)
            if (!reader.F32(v)) matrixOk = false;
        if (!matrixOk || sample.bone < 0 || !ValidMatrix(sample.scale))
            return PmmDecode::BadData;
        if (!keySeen.insert({sample.bone, sample.frame}).second)
            return PmmDecode::BadData;
        out.keys.push_back(sample);
    }
    // Version 2: the curve array follows the whole version 1 layout, four
    // bytes per key record and in the same order.  A curve byte outside the
    // 0..127 control-point domain is refused like any other bad field.
    if (withCurves) {
        for (std::size_t i = 0; i < out.keys.size(); ++i) {
            std::uint8_t bytes[kPmmCurveBytes] = {};
            if (!reader.Bytes(bytes, sizeof bytes) || !ValidCurveBytes(bytes))
                return PmmDecode::BadData;
            for (std::size_t j = 0; j < kPmmCurveBytes; ++j)
                out.keys[i].curve[j] = bytes[j];
        }
    }
    return PmmDecode::Ok;
}

}  // namespace

// No record key at hand: the exact size picks the layout.  Module paths never
// take this route - they know the suffix and call the version overload, so a
// truncated version 2 record is refused rather than read as version 1.
PmmDecode DecodePmm(const std::uint8_t* data, std::size_t size, Samples& out) {
    return DecodePmmLayout(data, size, 0, out);
}

PmmDecode DecodePmm(const std::uint8_t* data, std::size_t size,
                    std::uint32_t version, Samples& out) {
    return DecodePmmLayout(data, size, version, out);
}

// ---- VMD tail extension payload codec -----------------------------------

std::vector<std::uint8_t> EncodeExtension(const VmdExtension& extension) {
    std::vector<std::uint8_t> out;
    std::set<std::pair<std::array<char, 15>, std::uint32_t>> seen;
    std::vector<const VmdExtension::Entry*> entries;
    bool withCurves = false;
    for (const VmdExtension::Entry& entry : extension.entries) {
        // A key that scaled back to 1 still carries the curve of the interval
        // that ends at it, so the curve alone is enough to keep the entry.
        if (!WritesKey(entry.scale, entry.curve)) continue;
        if (!seen.insert({entry.name, entry.frame}).second) continue;
        if (!bone_scale::Linear(entry.curve)) withCurves = true;
        entries.push_back(&entry);
    }
    // The version is the entry stride: version 1 entries are 55 bytes, version
    // 2 entries are 59.  With no curve anywhere the payload is byte-for-byte
    // the version 1 payload.
    out.insert(out.end(), kPayloadMagic, kPayloadMagic + sizeof kPayloadMagic);
    PutU32(out, withCurves ? kExtensionVersionV2 : kExtensionVersion);
    PutU32(out, 0);
    PutU64(out, extension.prefixBytes);
    PutU32(out, extension.prefixCrc32);
    PutU32(out, static_cast<std::uint32_t>(entries.size()));
    for (const VmdExtension::Entry* entry : entries) {
        out.insert(out.end(), entry->name.begin(), entry->name.end());
        PutU32(out, entry->frame);
        for (float v : entry->scale.v) PutF32(out, v);
        if (withCurves)
            out.insert(out.end(), entry->curve.begin(), entry->curve.end());
    }
    PutU32(out, Crc32Of(out.data(), out.size()));
    return out;
}

ExtensionDecode DecodeExtension(const std::uint8_t* data, std::size_t size,
                                VmdExtension& out) {
    out = {};
    if (data == nullptr || size < kPayloadHeaderBytes + 4 ||
        size > kMaxExtensionBytes)
        return ExtensionDecode::BadData;
    if (std::memcmp(data, kPayloadMagic, sizeof kPayloadMagic) != 0 &&
        std::memcmp(data, kLegacyPayloadMagic, sizeof kLegacyPayloadMagic) != 0)
        return ExtensionDecode::BadMagic;
    Reader reader{data, size, 8};
    std::uint32_t version = 0, reserved = 0, crc = 0, count = 0;
    std::uint64_t prefixBytes = 0;
    if (!reader.U32(version) || !reader.U32(reserved) ||
        !reader.U64(prefixBytes) || !reader.U32(crc) || !reader.U32(count))
        return ExtensionDecode::BadData;
    // Both known versions are read; anything else keeps the existing refusal.
    if (version != kExtensionVersion && version != kExtensionVersionV2)
        return ExtensionDecode::BadVersion;
    if (reserved != 0) return ExtensionDecode::BadData;
    (void)crc;
    if (count > kMaxExtensionEntries) return ExtensionDecode::BadData;
    const bool withCurves = version == kExtensionVersionV2;
    const std::size_t entryBytes =
        withCurves ? kExtensionEntryBytesV2 : kExtensionEntryBytes;
    const std::uint64_t expected =
        kPayloadHeaderBytes + std::uint64_t(count) * entryBytes + 4;
    if (expected != static_cast<std::uint64_t>(size))
        return ExtensionDecode::BadData;
    const std::uint32_t stored = [&]() {
        std::uint32_t value = 0;
        std::memcpy(&value, data + size - 4, 4);
        return value;
    }();
    if (stored != Crc32Of(data, size - 4)) return ExtensionDecode::BadCrc;
    out.prefixBytes = prefixBytes;
    out.prefixCrc32 = crc;
    out.entries.reserve(count);
    std::set<std::pair<std::array<char, 15>, std::uint32_t>> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
        VmdExtension::Entry entry;
        if (!reader.Bytes(entry.name.data(), entry.name.size()) ||
            !reader.U32(entry.frame))
            return ExtensionDecode::BadData;
        bool matrixOk = true;
        for (float& v : entry.scale.v)
            if (!reader.F32(v)) matrixOk = false;
        if (!matrixOk || !ValidMatrix(entry.scale)) return ExtensionDecode::BadData;
        if (withCurves) {
            std::uint8_t bytes[4] = {};
            if (!reader.Bytes(bytes, sizeof bytes) || !ValidCurveBytes(bytes))
                return ExtensionDecode::BadData;
            for (std::size_t j = 0; j < 4; ++j) entry.curve[j] = bytes[j];
        }
        if (!seen.insert({entry.name, entry.frame}).second)
            return ExtensionDecode::BadData;
        out.entries.push_back(entry);
    }
    return ExtensionDecode::Ok;
}

// ---- VMD tail extension files -------------------------------------------

void PrefixFingerprint::Append(const void* data, std::size_t size) {
    state_ = Crc32Update(state_, data, size);
    bytes_ += size;
}

std::uint32_t PrefixFingerprint::crc32() const {
    return state_ ^ 0xFFFFFFFFu;
}

ExtensionSave SaveVmdExtension(int fd, const VmdExtension& extension) {
    if (extension.entries.empty()) return ExtensionSave::Untouched;
    const auto position = _telli64(fd);
    if (position < 0 || static_cast<std::uint64_t>(position) != extension.prefixBytes)
        return ExtensionSave::Failed;
    const std::vector<std::uint8_t> payload = EncodeExtension(extension);
    if (payload.size() > kMaxExtensionBytes) {
        // Never truncate: the caller reports the failure and the user keeps
        // the data instead of a silently shortened extension.
        runtime_log::Write(
            "bone scale: VMD extension is %llu bytes, over the %llu-byte "
            "limit; the motion save will fail visibly instead of dropping it",
            static_cast<unsigned long long>(payload.size()),
            static_cast<unsigned long long>(kMaxExtensionBytes));
        return ExtensionSave::Failed;
    }
    std::vector<std::uint8_t> tail;
    tail.reserve(payload.size() + kFooterBytes);
    tail.insert(tail.end(), payload.begin(), payload.end());
    PutU64(tail, static_cast<std::uint64_t>(payload.size()));
    tail.insert(tail.end(), kFooterMagic, kFooterMagic + sizeof kFooterMagic);
    std::size_t done = 0;
    while (done < tail.size()) {
        const unsigned int chunk = static_cast<unsigned int>(
            (tail.size() - done) > 0x100000u ? 0x100000u : (tail.size() - done));
        const int wrote = _write(fd, tail.data() + done, chunk);
        if (wrote != static_cast<int>(chunk)) return ExtensionSave::Failed;
        done += static_cast<std::size_t>(wrote);
    }
    return ExtensionSave::Written;
}

// Streams the first `bytes` bytes of the open file and returns their CRC32.
bool PrefixCrc32(int fd, std::uint64_t bytes, std::uint32_t& out) {
    if (_lseeki64(fd, 0, SEEK_SET) != 0) return false;
    std::uint32_t crc = 0xFFFFFFFFu;
    std::uint64_t remaining = bytes;
    std::vector<std::uint8_t> buffer(64u * 1024u);
    while (remaining > 0) {
        const unsigned int chunk = static_cast<unsigned int>(
            remaining > buffer.size() ? buffer.size() : remaining);
        const int got = _read(fd, buffer.data(), chunk);
        if (got != static_cast<int>(chunk)) return false;
        crc = Crc32Update(crc, buffer.data(), chunk);
        remaining -= chunk;
    }
    out = crc ^ 0xFFFFFFFFu;
    return true;
}

ExtensionLoad LoadVmdExtension(const std::wstring& vmdPath, VmdExtension& out,
                               std::int64_t expectedPrefixBytes) {
    out = {};
    int fd = -1;
    if (_wsopen_s(&fd, vmdPath.c_str(), 0x8000 /*_O_BINARY*/,
                  0x40 /*_O_RDONLY*/, 0x80 /*_S_IREAD*/) != 0)
        return ExtensionLoad::Absent;
    const __int64 length = _filelengthi64(fd);
    if (length < static_cast<__int64>(kFooterBytes)) {
        _close(fd);
        return ExtensionLoad::Absent;
    }
    // _lseeki64 returns the new offset on success and -1 on failure.
    if (_lseeki64(fd, length - static_cast<__int64>(kFooterBytes),
                  SEEK_SET) < 0) {
        _close(fd);
        return ExtensionLoad::Ignored;
    }
    std::uint8_t footer[kFooterBytes] = {};
    if (_read(fd, footer, sizeof footer) != static_cast<int>(sizeof footer)) {
        _close(fd);
        runtime_log::Write(
            "bone scale: VMD extension ignored (unreadable footer)");
        return ExtensionLoad::Ignored;
    }
    std::uint64_t payloadBytes = 0;
    std::memcpy(&payloadBytes, footer, sizeof payloadBytes);
    if (std::memcmp(footer + 8, kFooterMagic, sizeof kFooterMagic) != 0 &&
        std::memcmp(footer + 8, kLegacyFooterMagic,
                    sizeof kLegacyFooterMagic) != 0) {
        _close(fd);
        return ExtensionLoad::Absent;  // an ordinary motion: no tail at all
    }
    // The footer is ours from here on: every failure is reported and the
    // extension is refused as a whole, never half applied.
    if (payloadBytes > kMaxExtensionBytes ||
        payloadBytes + kFooterBytes > static_cast<std::uint64_t>(length)) {
        _close(fd);
        runtime_log::Write(
            "bone scale: VMD extension ignored (payload size %llu does not fit "
            "the file)",
            static_cast<unsigned long long>(payloadBytes));
        return ExtensionLoad::Ignored;
    }
    const std::uint64_t prefixBytes =
        static_cast<std::uint64_t>(length) - payloadBytes - kFooterBytes;
    // The caller passes the end offset of the standard part as it read it
    // (the _telli64 of the read descriptor), so the boundary is verified
    // against more than the tail's own claim.
    if (expectedPrefixBytes >= 0 &&
        prefixBytes != static_cast<std::uint64_t>(expectedPrefixBytes)) {
        out = {};
        _close(fd);
        runtime_log::Write(
            "bone scale: VMD extension ignored (standard boundary %llu does "
            "not match the %llu bytes the loader actually read)",
            static_cast<unsigned long long>(prefixBytes),
            static_cast<unsigned long long>(expectedPrefixBytes));
        return ExtensionLoad::Ignored;
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payloadBytes));
    if (_lseeki64(fd, static_cast<__int64>(prefixBytes), SEEK_SET) < 0 ||
        (!payload.empty() &&
         _read(fd, payload.data(),
               static_cast<unsigned int>(payload.size())) !=
             static_cast<int>(payload.size()))) {
        _close(fd);
        runtime_log::Write(
            "bone scale: VMD extension ignored (unreadable payload)");
        return ExtensionLoad::Ignored;
    }
    const ExtensionDecode decode =
        DecodeExtension(payload.data(), payload.size(), out);
    if (decode != ExtensionDecode::Ok) {
        out = {};
        _close(fd);
        runtime_log::Write(
            "bone scale: VMD extension ignored (payload magic, version, CRC "
            "or entry check failed, code %d)",
            static_cast<int>(decode));
        return ExtensionLoad::Ignored;
    }
    // The payload must sit exactly between the standard prefix and the
    // footer, and the prefix must still be the bytes it was written with.
    if (out.prefixBytes != prefixBytes) {
        out = {};
        _close(fd);
        runtime_log::Write(
            "bone scale: VMD extension ignored (prefix length does not align "
            "with the file size)");
        return ExtensionLoad::Ignored;
    }
    std::uint32_t prefixCrc = 0;
    if (!PrefixCrc32(fd, prefixBytes, prefixCrc) ||
        prefixCrc != out.prefixCrc32) {
        out = {};
        _close(fd);
        runtime_log::Write(
            "bone scale: VMD extension ignored (the standard part of the "
            "motion no longer matches its recorded CRC)");
        return ExtensionLoad::Ignored;
    }
    _close(fd);
    return ExtensionLoad::Loaded;
}

// ---- imported-key matching ----------------------------------------------

std::vector<Assignment> MatchImported(
    const VmdExtension& extension, const std::vector<ImportedKey>& imported,
    const std::vector<std::array<char, 20>>& boneNames,
    std::int64_t frameOffset) {
    std::vector<Assignment> result;
    if (frameOffset < 0) return result;  // a negative import offset is refused
    // One index pass per call: bone name -> bone (-1 = several bones share
    // the name) and (bone, target frame) -> key slot (-1 = ambiguous).
    std::map<std::string, int> boneByName;
    for (std::size_t i = 0; i < boneNames.size(); ++i) {
        const std::string name =
            FieldName(boneNames[i].data(), boneNames[i].size());
        const auto it = boneByName.find(name);
        if (it == boneByName.end())
            boneByName.emplace(name, static_cast<int>(i));
        else
            it->second = -1;
    }
    std::map<std::pair<int, std::uint64_t>, int> slotByTarget;
    for (const ImportedKey& key : imported) {
        if (key.slot < 0) continue;
        const std::pair<int, std::uint64_t> target{key.bone,
                                                   std::uint64_t{key.frame}};
        const auto it = slotByTarget.find(target);
        if (it == slotByTarget.end())
            slotByTarget.emplace(target, key.slot);
        else
            it->second = -1;
    }
    for (const VmdExtension::Entry& entry : extension.entries) {
        const auto bone = boneByName.find(VmdName(entry.name));
        if (bone == boneByName.end() || bone->second < 0) continue;
        // 64-bit sum: a large offset can never wrap into a real frame.
        const std::uint64_t target =
            static_cast<std::uint64_t>(frameOffset) +
            static_cast<std::uint64_t>(entry.frame);
        const auto slot =
            slotByTarget.find({bone->second, target});
        if (slot == slotByTarget.end() || slot->second < 0) continue;
        Assignment assignment;
        assignment.slot = slot->second;
        assignment.scale = entry.scale;
        assignment.curve = entry.curve;
        result.push_back(assignment);
    }
    return result;
}

// ---- model-facing glue ---------------------------------------------------

void CapturePmm(const MMDApp& app, pmm_extension::Document& document) {
    std::map<int, SlotBaseline>& baselines = Baselines();
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        const std::string keyV1 = PmmKey(slot, kPmmVersionV1);
        const std::string keyV2 = PmmKey(slot, kPmmVersionV2);

        // v1 and v2 are the two layouts this reader knows; every other suffix
        // is an unknown version and keeps its raw bytes until a real edit.
        std::vector<std::string> unknown;
        for (const pmm_extension::Record& entry : document.records) {
            int other = -1;
            std::string version;
            if (!ParsePmmKey(entry.key, other, version) || other != slot)
                continue;
            if (version != "v1" && version != "v2")
                unknown.push_back(entry.key);
        }
        // The record of this slot, whichever of the two known versions the
        // document holds.  Its suffix is the layout promise every decode below
        // has to honour.
        std::uint32_t existingVersion = 0;
        const pmm_extension::Record* existing =
            FindSlotRecord(document, slot, existingVersion);
        const bool hadRecord = existing != nullptr;
        unsigned char* model = app.ModelSlot(slot);
        if (model == nullptr) {
            // The slot is empty now: nothing may inherit what a previous
            // model of this slot left in the project.
            for (const std::string& name : unknown)
                pmm_extension::Erase(document, name);
            pmm_extension::Erase(document, keyV1);
            pmm_extension::Erase(document, keyV2);
            baselines.erase(slot);
            continue;
        }
        const Samples current = GatherSamples(model);
        const bool empty = current.pose.empty() && current.keys.empty();
        // The suffix and the payload describe the same version, so both come
        // from one question: does this capture carry a curve?  "v2" for a
        // capture with a key curve, "v1" otherwise.
        const std::string key = PmmHasCurves(current) ? keyV2 : keyV1;
        const std::string stale = key == keyV1 ? keyV2 : keyV1;
        const auto baseline = baselines.find(slot);
        const std::uint64_t fingerprint = BoneFingerprint(model);
        const bool sameModel = baseline != baselines.end() &&
                               baseline->second.model == model &&
                               baseline->second.boneFingerprint == fingerprint;
        if (!sameModel) {
            // A different model occupies the slot now; its predecessor's
            // record would be handed over as this model's own scale.
            for (const std::string& name : unknown)
                pmm_extension::Erase(document, name);
            pmm_extension::Erase(document, keyV1);
            pmm_extension::Erase(document, keyV2);
            if (!empty) pmm_extension::Set(document, key, EncodePmm(current));
            continue;
        }
        // A real edit moves bone_scale::Revision (SetPose/SetKey/SetKeyCurve/
        // Restore/Undo/Redo) or reshuffles the frames of the scaled keys
        // (frame edits never touch the revision).  Seeking rebuilds the
        // interpolated pose through Evaluate and does neither, so it is not
        // an edit.
        const bool edited =
            baseline->second.revision != bone_scale::Revision(model) ||
            baseline->second.keyFrames != KeyFrames(current);
        if (edited) {
            // The unknown versions of this slot no longer describe the model
            // and may be replaced.
            for (const std::string& name : unknown)
                pmm_extension::Erase(document, name);
            if (empty) {
                pmm_extension::Erase(document, keyV1);
                pmm_extension::Erase(document, keyV2);
                continue;
            }
        }
        // Refusing a record does not mean the user cleared its scale data.
        if (baseline->second.refused && !edited && hadRecord) {
            Samples stored;
            if (DecodePmm(existing->value.data(), existing->value.size(),
                          existingVersion, stored) == PmmDecode::Ok)
                continue;
        }
        if (empty) {
            Samples stored;
            if (hadRecord &&
                DecodePmm(existing->value.data(), existing->value.size(),
                          existingVersion, stored) == PmmDecode::Ok) {
                pmm_extension::Erase(document, keyV1);
                pmm_extension::Erase(document, keyV2);
            }
            continue;
        }
        if (hadRecord && !edited) {
            Samples stored;
            if (DecodePmm(existing->value.data(), existing->value.size(),
                          existingVersion, stored) != PmmDecode::Ok)
                continue;  // an unparseable record keeps its raw bytes
        }
        // The known record is rebuilt from the current pose and keys on every
        // capture (a failed save is retried with the same completeness); only
        // a record this reader cannot parse keeps its bytes until an edit.
        // The record is written whole - pmm_extension::Representable checks
        // the document budget in pmm_save before the file is opened.  A record
        // of the other known version is dropped so the slot holds one layout.
        pmm_extension::Erase(document, stale);
        pmm_extension::Set(document, key, EncodePmm(current));
    }
}

void LoadPmm(const MMDApp& app, const pmm_extension::Document& document) {
    Baselines().clear();
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        unsigned char* model = app.ModelSlot(slot);
        if (model == nullptr) continue;
        bool refused = false;
        std::uint32_t recordVersion = 0;
        const pmm_extension::Record* record =
            FindSlotRecord(document, slot, recordVersion);
        if (record != nullptr) {
            Samples samples;
            if (DecodePmm(record->value.data(), record->value.size(),
                          recordVersion, samples) != PmmDecode::Ok) {
                runtime_log::Write(
                    "bone scale: PMM record of model slot %d is malformed and "
                    "was left in the project unchanged",
                    slot);
            } else {
                bone_scale::Snapshot snapshot;
                if (BuildSnapshot(model, samples, snapshot)) {
                    bone_scale::Restore(model, snapshot);
                } else {
                    refused = true;
                    runtime_log::Write(
                        "bone scale: PMM record of model slot %d does not fit "
                        "the loaded model and was left in the project "
                        "unchanged",
                        slot);
                }
            }
        }
        // The baseline is taken after Restore (which may advance the
        // revision), so everything applied here counts as loaded, not edited.
        // It is never moved by a capture: only a successful save updates the
        // project document, and the module cannot observe that.
        SlotBaseline& baseline = Baselines()[slot];
        baseline.model = model;
        baseline.revision = bone_scale::Revision(model);
        baseline.boneFingerprint = BoneFingerprint(model);
        baseline.keyFrames = KeyFrames(GatherSamples(model));
        baseline.refused = refused;
    }
}

void ResetPmm() {
    Baselines().clear();
}

void CaptureExtension(const unsigned char* model, std::uint32_t minFrame,
                      VmdExtension& out) {
    out.entries.clear();
    if (model == nullptr) return;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const int boneCount = static_cast<int>(record.boneCount);
    const mdl::BoneRecord* bones = record.boneTable;
    const mdl::BoneKey* keys = record.boneKeys;
    if (boneCount <= 0 || bones == nullptr || keys == nullptr) return;
    std::set<std::pair<std::array<char, 15>, std::uint32_t>> seen;
    // Runtime pool capacity: the index sweep and the owner table both cover it,
    // or a key the pool grew into would be dropped from the VMD extension.
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(record);
    KeyOwners owners(keys, boneCount, boneKeyCapacity);
    for (unsigned int index = 0;
         index < static_cast<unsigned int>(boneKeyCapacity); ++index) {
        if (keys[index].allocated == 0) continue;
        const bone_scale::Matrix scale = bone_scale::Key(model, static_cast<int>(index));
        // The curve is read even when the scale has gone back to 1: that key
        // owns the interval that ends on it, and dropping it here is what made
        // a scale2 -> scale1 motion lose its end curve.
        const bone_scale::Curve curve = bone_scale::KeyCurve(model, static_cast<int>(index));
        if (!WritesKey(scale, curve)) continue;
        const int bone = owners.Get(index);
        if (bone < 0) continue;
        VmdExtension::Entry entry;
        if (extended_names::exportingUnicodeMotion) entry.name = extended_names::MotionIndexName(static_cast<std::uint32_t>(bone));
        else std::memcpy(entry.name.data(), bones[bone].name, entry.name.size());
        entry.frame = keys[index].frame - minFrame;
        entry.scale = scale;
        // The key's curve goes with its matrix, so a version 1 payload stays
        // the payload of a motion without curves.
        entry.curve = curve;
        if (!seen.insert({entry.name, entry.frame}).second) continue;
        out.entries.push_back(entry);
    }
}

int ApplyImportedExtension(unsigned char* model, const VmdExtension& extension,
                           int frameOffset) {
    if (model == nullptr || extension.entries.empty()) return 0;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const int boneCount = static_cast<int>(record.boneCount);
    const mdl::BoneRecord* bones = record.boneTable;
    const mdl::BoneKey* keys = record.boneKeys;
    if (boneCount <= 0 || bones == nullptr || keys == nullptr) return 0;
    std::vector<std::array<char, 20>> boneNames(
        static_cast<std::size_t>(boneCount));
    if (const auto* targets = extended_names::importedBoneTargets) {
        for (std::size_t source=0; source<targets->size(); ++source) {
            const int target=(*targets)[source];
            if (target<0 || target>=boneCount) continue;
            const auto name=extended_names::MotionIndexName(static_cast<std::uint32_t>(source));
            std::memcpy(boneNames[target].data(),name.data(),name.size());
        }
    } else {
    for (int i = 0; i < boneCount; ++i)
        std::memcpy(boneNames[static_cast<std::size_t>(i)].data(), bones[i].name,
                    boneNames[static_cast<std::size_t>(i)].size());
    }
    std::vector<ImportedKey> imported;
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(record);
    KeyOwners owners(keys, boneCount, boneKeyCapacity);
    for (unsigned int index = 0;
         index < static_cast<unsigned int>(boneKeyCapacity); ++index) {
        if (keys[index].allocated == 0) continue;
        const int bone = owners.Get(index);
        if (bone < 0) continue;
        ImportedKey key;
        key.bone = bone;
        key.frame = keys[index].frame;
        key.slot = static_cast<int>(index);
        imported.push_back(key);
    }
    const std::vector<Assignment> assignments = MatchImported(
        extension, imported, boneNames, static_cast<std::int64_t>(frameOffset));
    for (const Assignment& assignment : assignments) {
        bone_scale::SetKey(model, assignment.slot, assignment.scale);
        // Matrix and curve travel together: an entry without a curve (any
        // version 1 entry) puts the slot back on the linear default, and an
        // entry whose scale returned to 1 still installs the curve of the
        // interval that ends at that key.  SetKey leaves the curve alone, so
        // the curve is written on its own and a linear one clears the slot.
        bone_scale::SetKeyCurve(model, assignment.slot, assignment.curve);
    }
    const int skipped =
        static_cast<int>(extension.entries.size()) - static_cast<int>(assignments.size());
    if (skipped > 0)
        runtime_log::Write(
            "bone scale: VMD extension applied %d of %d entries (missing or "
            "ambiguous bone/key skipped)",
            static_cast<int>(assignments.size()),
            static_cast<int>(extension.entries.size()));
    return static_cast<int>(assignments.size());
}

}  // namespace bone_scale_io
}  // namespace mikumikustudio
