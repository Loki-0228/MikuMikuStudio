#include "mikudancestudio/extended_names.hpp"
// MAINTENANCE: native morph (expression) curves for one model.
//
// What this module owns
// ---------------------
// The model's own morph animation is the original 20-byte linked MorphKey pool
// (model+0x2798, head index == morph index, one linear weight per record).  The
// curve editor works in animation_curve::Track.  This module reads a native
// track as a curve, mirrors an authored exact curve back into the native keys,
// evaluates the exact curve every frame, and persists it in the PMM v2
// extension document and in a block appended to the model VMD.  The public
// contract and both wire layouts are documented in morph_curve.hpp.
//
// Access points wired into the port (see the report that accompanies this
// change; nothing else calls into this module):
//   src/model/model_frame_seek.cpp      SeekModelFrame   -> Evaluate()
//   src/model/model_keyframe_advance.cpp AdvanceModelKeyframes -> Evaluate()
//   src/model/model_dispose.cpp         ModelDispose     -> Forget()
//   src/model/model_init.cpp            ModelInitDefaults-> Forget()
//   src/io/pmm_project_settings.cpp     Reset/Load/Capture -> Reset/LoadPmm/CapturePmm
//   src/io/vmd_save.cpp                 SaveVmdFile      -> CaptureVmd() + Wr()
//   src/model/vmd_load.cpp              LoadVmdMotion    -> VmdBlockSize() + ApplyVmd()
//
// Where the VMD block sits
// ------------------------
// The block is written after the complete standard body and BEFORE the
// bone-scale extension payload, both through the same write lambda:
//
//   [standard VMD body][morph curve block][bone scale payload][MMSXVMDX footer]
//   (legacy MDSXVMDX footers are also read)
//
// bone_scale_io defines the container's standard prefix as the byte count the
// loader consumed before it calls LoadVmdExtension, so a loader that steps over
// this block first keeps that container's own boundary and CRC intact, and the
// block is never written into the standard morph section (which stays readable
// for an original reader).  A file without authored curves is byte-identical to
// what the port wrote before this module existed.
//
// Native authority, and why nothing is destroyed
// ----------------------------------------------
// The native pool always wins at the frames a user edits, but the authored
// curve is never edited in place.  Two layers are kept per morph: the pristine
// `authored` curve together with the native chain it was installed with
// (`mirror`), and a derived `effective` curve:
//
//   effective = authored, minus the authored keys whose mirrored native record
//               is gone, plus one linear native key for every native record the
//               mirror did not have or whose value moved - and the authored ease
//               of every interval such an edit lands in falls back to the native
//               interpolation.
//
// Registration, deletion, movement, drag editing and the channel bake therefore
// bend the effective curve without touching the author's data, and as soon as
// the pool again matches the mirror the diff is empty and the authored curve,
// tangents included, is in force again.  That is what makes a host undo (or a
// user deleting the extra key) restore custom tangents without this module
// hooking the undo ring at all.  What plays, what Read returns and what both
// savers write is the effective layer, so a save matches what was heard.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikudancestudio/morph_curve.hpp"

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/ui_language.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mikudancestudio::morph_curve {
namespace {

namespace ac = animation_curve;

constexpr char kPmmPrefix[] = "org.mikudancestudio.model.";
constexpr char kPmmMiddle[] = ".morphCurve.";
constexpr char kPmmVersion[] = "v1";
constexpr char kPayloadMagic[4] = {'M', 'M', 'S', 'C'};
constexpr std::uint16_t kPayloadVersion = 1;
constexpr std::size_t kPayloadHeaderBytes = 12;
constexpr char kVmdBlockMagic[8] = {'M', 'M', 'S', 'M', 'C', 'R', 'V', '1'};
constexpr std::size_t kVmdBlockHeaderBytes = 16;
constexpr double kHandleWeight = 1.0 / 3.0;

// ---------------------------------------------------------------------------
// Bytes
// ---------------------------------------------------------------------------

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & std::uint32_t(-int(crc & 1)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

bool GetU16(const std::uint8_t* data, std::size_t size, std::size_t& at, std::uint16_t& out) {
    if (at + 2 > size) return false;
    out = static_cast<std::uint16_t>(data[at] | (std::uint16_t(data[at + 1]) << 8));
    at += 2;
    return true;
}

bool GetU32(const std::uint8_t* data, std::size_t size, std::size_t& at, std::uint32_t& out) {
    if (at + 4 > size) return false;
    out = std::uint32_t(data[at]) | (std::uint32_t(data[at + 1]) << 8) |
          (std::uint32_t(data[at + 2]) << 16) | (std::uint32_t(data[at + 3]) << 24);
    at += 4;
    return true;
}

// ---------------------------------------------------------------------------
// Morph identity and stable ids
// ---------------------------------------------------------------------------

constexpr std::size_t kNameBytes = 20;

std::string HexEncode(const char* bytes, std::size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char byte = static_cast<unsigned char>(bytes[index]);
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0Fu]);
    }
    return out;
}

bool HexDecode(const std::string& text, std::size_t bytes, std::string& out) {
    if (text.size() != bytes * 2) return false;
    out.assign(bytes, '\0');
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < bytes; ++index) {
        const int high = digit(text[index * 2]);
        const int low = digit(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[index] = static_cast<char>((high << 4) | low);
    }
    return true;
}

std::wstring Localized(const MMDApp& app, const wchar_t* ja, const wchar_t* en,
                       const wchar_t* zh) {
    const unsigned language = app.EnglishUI();
    return language == 0 ? std::wstring(ja) : language == 1 ? std::wstring(en) : std::wstring(zh);
}

std::wstring Widened(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), length);
    return out;
}

// The morph table fingerprint: the count, every twenty-byte name, the panel and
// the type.  A different model at a recycled address fails it.
std::uint64_t MorphTableDigest(const unsigned char* model) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    std::uint64_t hash = 1469598103934665603ull;
    const auto feed = [&hash](const void* bytes, std::size_t size) {
        const auto* data = static_cast<const std::uint8_t*>(bytes);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= data[index];
            hash *= 1099511628211ull;
        }
    };
    feed(&record.morphCount, sizeof(record.morphCount));
    const mdl::MorphRecord* morphs = record.morphs;
    if (morphs == nullptr) return hash;
    for (std::uint32_t index = 0; index < record.morphCount; ++index) {
        feed(morphs[index].name, sizeof(morphs[index].name));
        feed(&morphs[index].panel, sizeof(morphs[index].panel));
        feed(&morphs[index].type, sizeof(morphs[index].type));
    }
    return hash;
}

struct Identity {
    const unsigned char* model = nullptr;
    const mdl::MorphRecord* morphs = nullptr;
    const mdl::MorphKey* keys = nullptr;
    std::uint64_t keyPoolIdentity = 0;
    std::uint32_t morphCount = 0;
    std::uint64_t digest = 0;
};

bool Matches(const Identity& identity, const unsigned char* model) {
    if (model == nullptr || identity.model != model) return false;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const bool samePool = identity.keyPoolIdentity
        ? identity.keyPoolIdentity == mdl::MorphKeyPoolIdentity(record.morphKeys)
        : identity.keys == record.morphKeys;
    if (identity.morphs != record.morphs || !samePool ||
        identity.morphCount != record.morphCount)
        return false;
    return identity.digest == MorphTableDigest(model);
}

Identity IdentityOf(const unsigned char* model) {
    Identity identity;
    identity.model = model;
    if (model == nullptr) return identity;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    identity.morphs = record.morphs;
    identity.keys = record.morphKeys;
    identity.keyPoolIdentity = mdl::MorphKeyPoolIdentity(record.morphKeys);
    identity.morphCount = record.morphCount;
    identity.digest = MorphTableDigest(model);
    return identity;
}

// ---------------------------------------------------------------------------
// Native tracks
// ---------------------------------------------------------------------------

struct NativeKey {
    std::int32_t frame = 0;
    double value = 0.0;
    // The export mark of the record ("selected" in the MMD timeline).  It is
    // read for the VMD block, which must carry exactly the keys the standard
    // morph section carries, but it is never part of a diff: marking keys does
    // not change what the curve is.
    bool allocated = false;
};

ac::Key NativeCurveKey(const NativeKey& source) {
    ac::Key key;
    key.frame = source.frame;
    key.value = source.value;
    key.inSlope = key.outSlope = 0.0;
    key.inWeight = key.outWeight = kHandleWeight;
    key.interpolation = ac::Interpolation::Linear;
    key.tangent = ac::Tangent::Free;
    return key;
}

// A key the native pool wrote: the morph track has no curve, so losing the
// authored ease is exactly what a native edit means.
void MakeNativeInterpolation(ac::Key& key) {
    key.inSlope = key.outSlope = 0.0;
    key.inWeight = key.outWeight = kHandleWeight;
    key.interpolation = ac::Interpolation::Linear;
    key.tangent = ac::Tangent::Free;
}

const NativeKey* FindNative(const std::vector<NativeKey>& keys, std::int32_t frame) {
    const auto found = std::lower_bound(
        keys.begin(), keys.end(), frame,
        [](const NativeKey& key, std::int32_t value) { return key.frame < value; });
    return found != keys.end() && found->frame == frame ? &*found : nullptr;
}

// Compares the shape that matters for the authored curve: frames and values.
// The export mark is deliberately excluded, so selecting keys in the timeline
// never invalidates a curve.
bool SameNative(const std::vector<NativeKey>& left, const std::vector<NativeKey>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].frame != right[index].frame || left[index].value != right[index].value)
            return false;
    }
    return true;
}

// Most evaluations see the same ordered native chain. Compare it in place;
// only a changed or noncanonical chain needs ReadChain's allocation, sorting
// and duplicate-frame normalization. Selection marks do not affect the shape.
bool MatchesNative(const mdl::MorphKey* keys, int morph, const std::vector<NativeKey>& observed) {
    const auto capacity = mdl::MorphKeyCapacity(keys);
    if (!keys || morph < 0 || std::size_t(morph) >= capacity || observed.empty()) return false;
    unsigned slot = unsigned(morph);
    for (std::size_t i=0; i<observed.size(); ++i) {
        if (slot >= capacity) return false;
        const auto& key = keys[slot];
        if (key.frame != std::uint32_t(observed[i].frame) || double(key.value) != observed[i].value) return false;
        if (!key.next) return i+1 == observed.size();
        slot = key.next;
    }
    return false;
}

// Walks one morph's chain from its head (index == morph).  Repeated frames are
// collapsed the way the VMD importer does (the record read last wins) and the
// result is put into frame order, so a damaged chain can never produce an
// invalid curve.
bool ReadChain(const mdl::MorphKey* keys, int morph, std::vector<NativeKey>& out,
               std::string& error) {
    out.clear();
    if (keys == nullptr) {
        error = "the model has no morph key pool";
        return false;
    }
    if (morph < 0 || static_cast<std::size_t>(morph) >= mdl::MorphKeyCapacity(keys)) {
        error = "morph index " + std::to_string(morph) + " is outside the morph key pool";
        return false;
    }
    unsigned index = static_cast<unsigned>(morph);
    const auto morphCapacity1 = mdl::MorphKeyCapacity(keys);
    for (std::size_t visited = 0; visited <= morphCapacity1; ++visited) {
        if (index >= morphCapacity1) {
            error = "the morph key chain leaves the pool";
            return false;
        }
        const mdl::MorphKey& record = keys[index];
        if (record.frame > static_cast<std::uint32_t>(kMaximumFrame)) {
            error = "morph key frame " + std::to_string(record.frame) + " is outside the supported range";
            return false;
        }
        NativeKey key;
        key.frame = static_cast<std::int32_t>(record.frame);
        key.value = static_cast<double>(record.value);
        key.allocated = record.allocated != 0;
        if (!std::isfinite(key.value)) {
            error = "morph key value is not finite";
            return false;
        }
        out.push_back(key);
        if (record.next == 0) break;
        index = record.next;
        if (visited == morphCapacity1) {
            error = "the morph key chain does not end";
            return false;
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const NativeKey& left, const NativeKey& right) { return left.frame < right.frame; });
    std::vector<NativeKey> unique;
    unique.reserve(out.size());
    for (const NativeKey& key : out) {
        if (!unique.empty() && unique.back().frame == key.frame) unique.back() = key;
        else unique.push_back(key);
    }
    out.swap(unique);
    if (out.empty()) {
        error = "the morph key chain is empty";
        return false;
    }
    return true;
}

ac::Track CurveOf(const std::vector<NativeKey>& keys) {
    ac::Track track;
    track.keys.reserve(keys.size());
    for (const NativeKey& key : keys) track.keys.push_back(NativeCurveKey(key));
    return track;
}

// The continuation slots of one chain (everything after the head).
void ChainContinuations(const mdl::MorphKey* keys, int morph, std::vector<unsigned>& out) {
    out.clear();
    if (keys == nullptr || morph < 0 || static_cast<std::size_t>(morph) >= mdl::MorphKeyCapacity(keys)) return;
    unsigned index = keys[morph].next;
    const auto morphCapacity2 = mdl::MorphKeyCapacity(keys);
    for (std::size_t visited = 0; visited <= morphCapacity2; ++visited) {
        if (index == 0 || index >= morphCapacity2) return;
        out.push_back(index);
        index = keys[index].next;
    }
}

// The export mark ("selected" in the MMD timeline): the one byte vmd_save
// filters the standard morph section on, and the same condition this module's
// VMD block must follow.
bool ChainHasAllocatedKey(const mdl::MorphKey* keys, int morph) {
    if (keys == nullptr || morph < 0 || static_cast<std::size_t>(morph) >= mdl::MorphKeyCapacity(keys)) return false;
    unsigned index = static_cast<unsigned>(morph);
    const auto morphCapacity3 = mdl::MorphKeyCapacity(keys);
    for (std::size_t visited = 0; visited <= morphCapacity3; ++visited) {
        if (index >= morphCapacity3) return false;
        if (keys[index].allocated != 0) return true;
        if (keys[index].next == 0) return false;
        index = keys[index].next;
    }
    return false;
}

// Physical occupancy: the chain carries at least one key record.  A continuation
// counts whatever its mark says, because the record itself is the key.  The
// frame-zero head counts only when it is marked, since that mark is the only
// thing that distinguishes a real frame-zero key from the untouched head every
// morph starts with.  Unmarking a key therefore never makes it disappear here.
bool ChainHasKeys(const mdl::MorphKey* keys, int morph) {
    if (keys == nullptr || morph < 0 || static_cast<std::size_t>(morph) >= mdl::MorphKeyCapacity(keys)) return false;
    const mdl::MorphKey& head = keys[morph];
    if (head.next != 0) return true;
    return head.frame == 0 && head.allocated != 0;
}

// ---------------------------------------------------------------------------
// Authored state: one pristine layer plus a derived effective layer
// ---------------------------------------------------------------------------
//
// `authored` is the curve exactly as the editor or a project file wrote it and
// nothing in this module ever edits it in place.  `mirror` is the native chain
// as it looked when that curve was installed and `observed` the chain seen at
// the last derive.  `effective` is derived from them:
//
//   effective = authored, minus the authored keys whose mirrored native record
//               is gone, plus one linear native key for every native record the
//               mirror did not have or whose value moved - and every authored
//               interval such an edit lands in loses its authored ease.
//
// The diff is always taken against the untouched mirror, so a native edit only
// bends the curve while it lasts: as soon as the pool matches the mirror again
// the diff is empty and the authored curve, tangents included, is in force
// again.  Any host that restores the pool gets the exact curve back without a
// single hook into its undo ring, and a user who deletes the extra key gets the
// same result.  What plays, what Read returns and what the savers write is the
// effective layer, so a save always matches what was heard.

struct State {
    Identity identity;
    std::map<int, ac::Track> authored;    // pristine curve per morph index
    std::map<int, ac::Track> effective;   // derived curve: what plays and saves
    std::map<int, std::vector<NativeKey>> mirror;    // chain when authored was installed
    std::map<int, std::vector<NativeKey>> observed;  // chain at the last derive
};

std::map<const unsigned char*, State>& States() {
    static std::map<const unsigned char*, State> states;
    return states;
}

State* Lookup(const unsigned char* model) {
    if (model == nullptr) return nullptr;
    auto found = States().find(model);
    if (found == States().end()) return nullptr;
    if (!Matches(found->second.identity, model)) {
        States().erase(found);
        return nullptr;
    }
    return &found->second;
}

// Installs one authored curve together with the native shape it mirrors, so the
// derive below can tell a native edit from the curve's own mirror.
void SetInstalled(State& state, int morph, const ac::Track& track,
                  const std::vector<NativeKey>& chain) {
    state.authored[morph] = track;
    state.effective[morph] = track;
    state.mirror[morph] = chain;
    state.observed[morph] = chain;
}

// Derives the effective curve of one morph.  Returns false when the pool still
// matches the mirror, in which case `out` is the authored curve unchanged - the
// exact restore path, which deliberately does not even touch the tangents.
bool Derive(const ac::Track& authored, const std::vector<NativeKey>& mirror,
            const std::vector<NativeKey>& fresh, ac::Track& out) {
    out = authored;
    if (SameNative(mirror, fresh)) return false;
    const auto compare = [](const ac::Key& key, std::int32_t frame) { return key.frame < frame; };
    // Interval ends whose authored ease the edits invalidate, collected by frame
    // value so the later inserts and erases cannot invalidate an index.
    std::set<std::int32_t> invalidated;
    // 1. A mirrored native frame that is gone takes its authored key with it;
    //    the interval that used to end there is gone as well.
    for (std::size_t index = 0; index < out.keys.size();) {
        const std::int32_t frame = out.keys[index].frame;
        if (FindNative(mirror, frame) != nullptr && FindNative(fresh, frame) == nullptr) {
            out.keys.erase(out.keys.begin() + static_cast<std::ptrdiff_t>(index));
            if (index < out.keys.size()) invalidated.insert(out.keys[index].frame);
        } else {
            ++index;
        }
    }
    // 2. A native key the mirror did not have, or whose value moved, wins at its
    //    own frame, and the authored ease of the interval it lands in goes too.
    for (const NativeKey& key : fresh) {
        const NativeKey* previous = FindNative(mirror, key.frame);
        if (previous != nullptr && previous->value == key.value) continue;
        auto at = std::lower_bound(out.keys.begin(), out.keys.end(), key.frame, compare);
        if (at != out.keys.end() && at->frame == key.frame) {
            *at = NativeCurveKey(key);
            // The interval that started here was eased for the old value.
            if (at + 1 != out.keys.end()) invalidated.insert((at + 1)->frame);
            continue;
        }
        if (at != out.keys.end()) invalidated.insert(at->frame);
        out.keys.insert(at, NativeCurveKey(key));
    }
    // 3. Everything an edit touched falls back to the native interpolation.
    for (ac::Key& key : out.keys) {
        if (invalidated.count(key.frame) != 0) MakeNativeInterpolation(key);
    }
    ac::UpdateAutomatic(out);
    return true;
}

// Brings the effective curves of a model back in step with its native pool.
// The authored layer is never modified here, so an edit only bends the curve
// for as long as it lasts.
void Sync(const unsigned char* model, State& state) {
    const mdl::MorphKey* keys = mdl::Mdl(model)->morphKeys;
    if (keys == nullptr) {
        state.authored.clear();
        state.effective.clear();
        state.mirror.clear();
        state.observed.clear();
        return;
    }
    for (auto entry = state.authored.begin(); entry != state.authored.end();) {
        const int morph = entry->first;
        auto mirror = state.mirror.find(morph);
        auto observed = state.observed.find(morph);
        if (mirror != state.mirror.end() && observed != state.observed.end() &&
            MatchesNative(keys, morph, observed->second)) {
            ++entry;
            continue;
        }
        std::vector<NativeKey> fresh;
        std::string error;
        if (!ReadChain(keys, morph, fresh, error)) {
            state.effective.erase(morph);
            state.mirror.erase(morph);
            state.observed.erase(morph);
            entry = state.authored.erase(entry);
            continue;
        }
        if (mirror == state.mirror.end() || observed == state.observed.end()) {
            // A state without its reference chain (which cannot be produced by
            // this module) treats the pool it sees now as the installed shape.
            state.mirror[morph] = fresh;
            state.observed[morph] = fresh;
            state.effective[morph] = entry->second;
            ++entry;
            continue;
        }
        if (SameNative(observed->second, fresh)) {
            ++entry;
            continue;
        }
        ac::Track derived;
        Derive(entry->second, mirror->second, fresh, derived);
        state.effective[morph] = std::move(derived);
        observed->second = std::move(fresh);
        ++entry;
    }
}

State* Current(const unsigned char* model) {
    State* state = Lookup(model);
    if (state == nullptr) return nullptr;
    Sync(model, *state);
    if (state->authored.empty()) {
        States().erase(model);
        return nullptr;
    }
    return state;
}

// ---------------------------------------------------------------------------
// Payload and block codec
// ---------------------------------------------------------------------------

bool EncodePayloadImpl(const std::vector<ac::NamedTrack>& tracks, std::vector<std::uint8_t>& out,
                       std::string& error) {
    out.clear();
    std::vector<std::uint8_t> curves;
    if (!ac::Encode(tracks, curves, error)) return false;
    if (curves.size() > kMaximumPayloadBytes - kPayloadHeaderBytes) {
        error = "the morph curve payload would need " + std::to_string(curves.size()) +
                " bytes, the limit is " +
                std::to_string(kMaximumPayloadBytes - kPayloadHeaderBytes);
        return false;
    }
    out.reserve(kPayloadHeaderBytes + curves.size());
    out.insert(out.end(), kPayloadMagic, kPayloadMagic + sizeof(kPayloadMagic));
    PutU16(out, kPayloadVersion);
    PutU16(out, 0);
    PutU32(out, static_cast<std::uint32_t>(curves.size()));
    out.insert(out.end(), curves.begin(), curves.end());
    return true;
}

bool DecodePayloadImpl(const std::uint8_t* data, std::size_t size,
                       std::vector<ac::NamedTrack>& out, std::string& error) {
    out.clear();
    if (data == nullptr || size < kPayloadHeaderBytes || size > kMaximumPayloadBytes) {
        error = "the morph curve payload has an unsupported size";
        return false;
    }
    if (std::memcmp(data, kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
        error = "the morph curve payload does not start with its magic";
        return false;
    }
    std::size_t at = sizeof(kPayloadMagic);
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t curveBytes = 0;
    if (!GetU16(data, size, at, version) || !GetU16(data, size, at, reserved) ||
        !GetU32(data, size, at, curveBytes)) {
        error = "the morph curve payload header is truncated";
        return false;
    }
    if (version != kPayloadVersion) {
        error = "unsupported morph curve payload version " + std::to_string(version);
        return false;
    }
    if (reserved != 0) {
        error = "the morph curve payload reserved field must be zero";
        return false;
    }
    if (curveBytes != size - at) {
        error = "the morph curve payload length does not match the data it holds";
        return false;
    }
    return ac::Decode(data + at, curveBytes, out, error);
}

// The block size the header declares.  Only the 16 header bytes are needed, so a
// loader can probe the bytes that follow a standard body before reading the
// body itself - which is exactly what src/model/vmd_load.cpp does.  Whether the
// declared block actually fits the buffer is the decoder's business, not this
// probe's: a loader reads the declared count and stops when the file ends early.
std::size_t VmdBlockSizeImpl(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < kVmdBlockHeaderBytes) return 0;
    if (std::memcmp(data, kVmdBlockMagic, sizeof(kVmdBlockMagic)) != 0) return 0;
    std::size_t at = sizeof(kVmdBlockMagic);
    std::uint32_t totalBytes = 0;
    if (!GetU32(data, size, at, totalBytes)) return 0;
    if (totalBytes < kVmdBlockHeaderBytes) return 0;
    if (totalBytes > kMaximumPayloadBytes + kVmdBlockHeaderBytes) return 0;
    return totalBytes;
}

}  // namespace

// ---------------------------------------------------------------------------
// Identity and enumeration
// ---------------------------------------------------------------------------

std::string StableId(int index, const char* name) {
    std::string id = "m" + std::to_string(index) + ":";
    if (name != nullptr) id += HexEncode(name, kNameBytes);
    return id;
}

bool ParseStableId(const std::string& id, int& index, std::string& name) {
    if (id.size() < 4 || id[0] != 'm') return false;
    const std::size_t colon = id.find(':');
    if (colon == std::string::npos || colon < 2) return false;
    const std::string digits = id.substr(1, colon - 1);
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') return false;
    }
    const long long value = std::strtoll(digits.c_str(), nullptr, 10);
    if (value < 0 || value > 1000000) return false;
    std::string raw;
    if (!HexDecode(id.substr(colon + 1), kNameBytes, raw)) return false;
    index = static_cast<int>(value);
    name = std::move(raw);
    return true;
}

const wchar_t* PanelLabel(std::uint8_t panel, unsigned language) {
    const bool english = language == 1;
    const bool chinese = language == 2;
    switch (panel) {
    case 1: return english ? L"Eyebrow" : chinese ? L"眉毛" : L"眉";
    case 2: return english ? L"Eye" : chinese ? L"眼睛" : L"目";
    case 3: return english ? L"Mouth" : chinese ? L"嘴" : L"口";
    case 4: return english ? L"Other" : chinese ? L"其他" : L"その他";
    default: return english ? L"System" : chinese ? L"系统" : L"システム";
    }
}

int PanelGroup(std::uint8_t panel) {
    return panel >= 1 && panel <= 4 ? static_cast<int>(panel) - 1 : -1;
}

std::vector<MorphInfo> Enumerate(const unsigned char* model, unsigned language) {
    std::vector<MorphInfo> out;
    if (model == nullptr) return out;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const mdl::MorphRecord* morphs = record.morphs;
    if (morphs == nullptr || record.morphCount == 0) return out;
    const mdl::MorphKey* keys = record.morphKeys;
    out.reserve(record.morphCount);
    for (std::uint32_t index = 0; index < record.morphCount; ++index) {
        MorphInfo info;
        info.index = static_cast<int>(index);
        info.panel = static_cast<std::uint8_t>(morphs[index].panel);
        info.id = StableId(info.index, morphs[index].name);
        info.label = text_encoding::MorphLabel(morphs[index], UiUsesEnglishNames(static_cast<unsigned char>(language)));
        if (info.label.empty() && morphs[index].name[0] != '\0') {
            // A Chinese model can carry a CP936 name in the CP932 field, which
            // MorphName() then fails to decode.  Fall back to the active code
            // page so the name still displays instead of disappearing.
            info.label = text_encoding::Display(morphs[index].name);
        }
        info.keyed = ChainHasKeys(keys, info.index);
        info.marked = ChainHasAllocatedKey(keys, info.index);
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<MorphInfo> Enumerate(const MMDApp& app, unsigned language) {
    return Enumerate(app.SelectedModel(), language);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool ReadNative(const unsigned char* model, int morph, ac::Track& out, std::string& error) {
    error.clear();
    out = ac::Track{};
    if (model == nullptr) {
        error = "there is no model";
        return false;
    }
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    if (morph < 0 || static_cast<std::uint32_t>(morph) >= record.morphCount) {
        error = "morph index " + std::to_string(morph) + " is outside the model";
        return false;
    }
    std::vector<NativeKey> keys;
    if (!ReadChain(record.morphKeys, morph, keys, error)) return false;
    out = CurveOf(keys);
    return true;
}

static bool ReadSynced(const unsigned char* model, const State* state, int morph, ac::Track& out, std::string& error) {
    if (state != nullptr) {
        const auto found = state->effective.find(morph);
        if (found != state->effective.end()) {
            error.clear();
            out = found->second;
            return true;
        }
    }
    return ReadNative(model, morph, out, error);
}

bool Read(const unsigned char* model, int morph, ac::Track& out, std::string& error) {
    return ReadSynced(model, Current(model), morph, out, error);
}

bool Read(const MMDApp& app, int morph, ac::Track& out, std::wstring& error) {
    unsigned char* const model = app.SelectedModel();
    if (model == nullptr) {
        out = ac::Track{};
        error = Localized(app, L"モデルが選択されていません。", L"No model is selected.",
                          L"没有选中模型。");
        return false;
    }
    std::string detail;
    if (!Read(model, morph, out, detail)) {
        error = Localized(app, L"表情トラックを読み込めませんでした。",
                          L"The morph track could not be read.", L"无法读取表情轨道。") +
                L" " + Widened(detail);
        return false;
    }
    error.clear();
    return true;
}

bool ReadMany(const MMDApp& app, const std::vector<int>& morphs,
              std::vector<ac::Track>& out, std::wstring& error) {
    out.clear();
    const auto* model = app.SelectedModel();
    if (!model) {
        error = Localized(app, L"モデルが選択されていません。", L"No model is selected.", L"没有选中模型。");
        return false;
    }
    const auto* state = Current(model);
    std::vector<ac::Track> tracks;tracks.reserve(morphs.size());
    for (int morph : morphs) {
        ac::Track track;std::string detail;
        if (!ReadSynced(model, state, morph, track, detail)) {
            error = Localized(app, L"表情トラックを読み込めませんでした。",
                L"The morph track could not be read.", L"无法读取表情轨道。") + L" " + Widened(detail);
            return false;
        }
        tracks.push_back(std::move(track));
    }
    out = std::move(tracks);error.clear();return true;
}

// These answer for the effective curve: a curve whose native keys were all
// removed is out of force (the pristine layer is retained and comes back if the
// pool is restored, but nothing is playing or saving from it right now).
bool HasCurve(const unsigned char* model, int morph) {
    const State* state = Lookup(model);
    return state != nullptr && state->effective.find(morph) != state->effective.end();
}

bool AnyCurve(const unsigned char* model) {
    const State* state = Lookup(model);
    return state != nullptr && !state->effective.empty();
}

std::size_t CurveCount(const unsigned char* model) {
    const State* state = Lookup(model);
    return state == nullptr ? 0 : state->effective.size();
}

bool HasCurve(const MMDApp& app, int morph) { return HasCurve(app.SelectedModel(), morph); }

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

namespace {

bool TrackMatchesModel(const mdl::ModelRecord& record, const ac::NamedTrack& named, int& morph,
                       std::string& error) {
    const mdl::MorphRecord* morphs = record.morphs;
    if (morphs == nullptr) {
        error = "the model has no morph table";
        return false;
    }
    std::string name;
    if (!ParseStableId(named.id, morph, name)) {
        error = "track id \"" + named.id + "\" is not a morph id";
        return false;
    }
    if (morph < 0 || static_cast<std::uint32_t>(morph) >= record.morphCount) {
        error = "track id \"" + named.id + "\" names a morph outside the model";
        return false;
    }
    if (name.size() != kNameBytes ||
        std::memcmp(name.data(), morphs[morph].name, kNameBytes) != 0) {
        error = "track id \"" + named.id + "\" does not name morph " + std::to_string(morph) +
                " of this model";
        return false;
    }
    return true;
}

bool TrackInvariants(const ac::NamedTrack& named, std::string& error) {
    if (named.track.discrete) {
        error = "a morph track cannot be discrete";
        return false;
    }
    if (!ac::Validate(named.track, error)) return false;
    if (named.track.keys.empty() || named.track.keys.front().frame != 0) {
        error = "keep a valid frame-zero key in every morph track";
        return false;
    }
    for (const ac::Key& key : named.track.keys) {
        if (key.frame > kMaximumFrame) {
            error = "frame " + std::to_string(key.frame) + " is outside the supported range";
            return false;
        }
    }
    return true;
}

}  // namespace

bool Write(unsigned char* model, const std::vector<ac::NamedTrack>& tracks, std::string& error) {
    error.clear();
    if (model == nullptr) {
        error = "there is no model";
        return false;
    }
    mdl::ModelRecord& record = *mdl::Mdl(model);
    mdl::MorphKey* keys = record.morphKeys;
    if (keys == nullptr || record.morphs == nullptr || record.morphCount == 0) {
        error = "the model has no morph track to edit";
        return false;
    }
    if (tracks.empty()) {
        error = "there is no morph track to write";
        return false;
    }

    // ---- validation: nothing is touched before every check passed ----------
    std::vector<std::pair<int, const ac::NamedTrack*>> ordered;
    ordered.reserve(tracks.size());
    std::set<int> seen;
    for (const ac::NamedTrack& named : tracks) {
        int morph = -1;
        if (!TrackMatchesModel(record, named, morph, error)) return false;
        if (!TrackInvariants(named, error)) {
            error = "track \"" + named.id + "\": " + error;
            return false;
        }
        if (!seen.insert(morph).second) {
            error = "morph " + std::to_string(morph) + " appears twice in the same write";
            return false;
        }
        ordered.emplace_back(morph, &named);
    }

    // ---- capacity: the heads are reused, everything else needs a free slot --
    std::size_t freeSlots = 0;
    const auto morphCapacity4 = mdl::MorphKeyCapacity(keys);
    for (std::size_t index = record.morphCount; index < morphCapacity4; ++index) {
        if (keys[index].frame == 0) ++freeSlots;
    }
    std::size_t needed = 0;
    std::size_t reused = 0;
    std::vector<unsigned> continuations;
    for (const auto& item : ordered) {
        ChainContinuations(keys, item.first, continuations);
        for (const unsigned index : continuations) {
            if (index < record.morphCount) {
                error = "the morph key chain of morph " + std::to_string(item.first) +
                        " leaves the continuation area";
                return false;
            }
            ++reused;
        }
        needed += item.second->track.keys.size() - 1;
    }
    if (needed > freeSlots + reused) {
        const std::uint64_t required = std::uint64_t(mdl::MorphKeyCapacity(keys)) +
            needed - freeSlots - reused;
        if (!mdl::MorphKeyPoolEnsure(record, required)) {
            error = "not enough memory or morph key indices for the write";
            return false;
        }
        keys = record.morphKeys;
    }

    // ---- transactional write ---------------------------------------------
    std::vector<mdl::MorphKey> snapshot;
    try {
        snapshot.assign(keys, keys + mdl::MorphKeyCapacity(keys));
    } catch (const std::bad_alloc&) {
        error = "not enough memory to snapshot the morph key pool";
        return false;
    }
    const std::uint32_t previousMaxFrame = record.maxFrame;
    const auto restore = [&]() {
        std::memcpy(keys, snapshot.data(), snapshot.size() * sizeof(mdl::MorphKey));
        record.maxFrame = previousMaxFrame;
    };

    // The mirror marks every key it writes.  The mark is the export selection
    // vmd_save filters the standard morph section on, so a key left unmarked
    // would be missing from the standard section - and therefore, since the
    // curve block carries exactly that section's frames, from the block as well:
    // the authored key would silently vanish on a VMD roundtrip.  Applying a
    // curve is an explicit "make these keys part of the motion" action, so it
    // selects them; a record that is not part of the curve is never touched (the
    // mirror only rewrites the morphs it is given).
    // Every replaced chain is released first, so a slot freed by one morph is
    // available to every morph that follows.
    for (const auto& item : ordered) {
        ChainContinuations(keys, item.first, continuations);
        for (const unsigned index : continuations) keys[index] = mdl::MorphKey{};
        keys[item.first] = mdl::MorphKey{};
    }

    std::size_t cursor = record.morphCount;
    bool complete = true;
    const auto writeCapacity = mdl::MorphKeyCapacity(keys);
    for (const auto& item : ordered) {
        const ac::Track& curve = item.second->track;
        unsigned previous = static_cast<unsigned>(item.first);
        keys[previous].frame = static_cast<std::uint32_t>(curve.keys.front().frame);
        keys[previous].value = static_cast<float>(curve.keys.front().value);
        keys[previous].allocated = 1;
        for (std::size_t index = 1; index < curve.keys.size(); ++index) {
            while (cursor < writeCapacity && keys[cursor].frame != 0) ++cursor;
            if (cursor >= writeCapacity) {
                complete = false;
                break;
            }
            const unsigned fresh = static_cast<unsigned>(cursor);
            keys[fresh] = mdl::MorphKey{};
            keys[fresh].frame = static_cast<std::uint32_t>(curve.keys[index].frame);
            keys[fresh].value = static_cast<float>(curve.keys[index].value);
            keys[fresh].allocated = 1;
            keys[fresh].previous = previous;
            keys[previous].next = fresh;
            previous = fresh;
            ++cursor;
        }
        if (!complete) break;
        const std::uint32_t last = static_cast<std::uint32_t>(curve.keys.back().frame);
        if (last > record.maxFrame) record.maxFrame = last;
    }
    if (!complete) {
        restore();
        error = "the morph key pool ran out of slots while writing";
        return false;
    }

    // ---- authoring state --------------------------------------------------
    State* state = Lookup(model);
    if (state == nullptr) {
        State fresh;
        fresh.identity = IdentityOf(model);
        state = &States().emplace(model, std::move(fresh)).first->second;
    } else {
        state->identity = IdentityOf(model);
    }
    for (const auto& item : ordered) {
        const int morph = item.first;
        std::vector<NativeKey> native;
        std::string reason;
        if (!ReadChain(keys, morph, native, reason)) {
            restore();
            error = reason;
            return false;
        }
        // The written curve becomes the pristine authored layer, mirrored by the
        // chain that was just rebuilt from it.
        SetInstalled(*state, morph, item.second->track, native);
    }
    // The free-slot hint of the registrar family is re-seeded exactly like
    // ResetMorphKeyCursor seeds it, so a key registered after this write can
    // still reach every slot this write freed.
    mdl::Mdl(model)->searchCursor = static_cast<std::int32_t>(record.morphCount);
    return true;
}

bool Write(MMDApp& app, const std::vector<ac::NamedTrack>& tracks, std::wstring& error) {
    unsigned char* const model = app.SelectedModel();
    if (model == nullptr) {
        error = Localized(app, L"モデルが選択されていません。", L"No model is selected.",
                          L"没有选中模型。");
        return false;
    }
    std::string detail;
    if (!Write(model, tracks, detail)) {
        error = Localized(app, L"表情カーブを書き込めませんでした。",
                          L"The morph curve could not be written.", L"无法写入表情曲线。") +
                L" " + Widened(detail);
        return false;
    }
    std::uint32_t last = app.LastRegisteredFrame();
    for (const ac::NamedTrack& named : tracks) {
        if (!named.track.keys.empty()) {
            last = std::max(last, static_cast<std::uint32_t>(named.track.keys.back().frame));
        }
    }
    app.LastRegisteredFrame() = last;
    app.SceneModified() = 1;
    // The pool was rewritten, so the incremental cursors of this model are
    // stale: re-seek, exactly like a VMD import does after registering keys.
    SeekModelFrame(model, static_cast<int>(app.state.currentFrame), app.PlaybackPhysicsMode());
    return true;
}

void Forget(unsigned char* model, int morph) {
    State* state = Lookup(model);
    if (state == nullptr) return;
    state->authored.erase(morph);
    state->effective.erase(morph);
    state->mirror.erase(morph);
    state->observed.erase(morph);
    if (state->authored.empty()) States().erase(model);
}

void Forget(unsigned char* model) {
    if (model == nullptr) {
        States().clear();
        return;
    }
    States().erase(model);
}

void Reset(MMDApp* app) {
    if (app == nullptr) {
        States().clear();
        return;
    }
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        if (unsigned char* model = app->ModelSlot(slot)) States().erase(model);
    }
}

void CopyState(const unsigned char* source, unsigned char* destination) {
    if (source == nullptr || destination == nullptr || source == destination) return;
    const State* const from = Lookup(source);
    Forget(destination);
    if (from == nullptr || from->authored.empty()) return;
    const mdl::MorphKey* const keys = mdl::Mdl(destination)->morphKeys;
    State& to = States()[destination];
    to.identity = IdentityOf(destination);
    for (const auto& entry : from->authored) {
        std::vector<NativeKey> native;
        std::string error;
        if (keys == nullptr || !ReadChain(keys, entry.first, native, error)) continue;
        const auto mirror = from->mirror.find(entry.first);
        // The copy keeps the pristine curve and the mirror it was installed
        // with; its own effective layer is derived from the chain it now sees,
        // so a copy that already carries native edits bends exactly like the
        // source does.
        const std::vector<NativeKey> reference =
            mirror != from->mirror.end() ? mirror->second : native;
        ac::Track derived;
        Derive(entry.second, reference, native, derived);
        to.authored[entry.first] = entry.second;
        to.effective[entry.first] = std::move(derived);
        to.mirror[entry.first] = reference;
        to.observed[entry.first] = std::move(native);
    }
    if (to.authored.empty()) States().erase(destination);
}

void Evaluate(unsigned char* model, double frame) {
    State* state = Current(model);
    if (state == nullptr || !std::isfinite(frame)) return;
    mdl::MorphRecord* const morphs = mdl::Morphs(model);
    if (morphs == nullptr) return;
    const std::uint32_t morphCount = mdl::Mdl(model)->morphCount;
    for (const auto& entry : state->effective) {
        const int morph = entry.first;
        if (morph < 0 || static_cast<std::uint32_t>(morph) >= morphCount) continue;
        const double value = ac::Evaluate(entry.second, frame).value;
        if (!std::isfinite(value)) continue;
        morphs[morph].value = static_cast<float>(value);
    }
}

// ---------------------------------------------------------------------------
// PMM v2 records
// ---------------------------------------------------------------------------

std::string PmmKey(int slot) {
    return std::string(kPmmPrefix) + std::to_string(slot) + kPmmMiddle + kPmmVersion;
}

bool ParsePmmKey(const std::string& key, int& slot, std::string& version) {
    static const std::string prefix = kPmmPrefix;
    static const std::string middle = kPmmMiddle;
    if (key.size() <= prefix.size() + middle.size()) return false;
    if (key.compare(0, prefix.size(), prefix) != 0) return false;
    std::size_t at = prefix.size();
    if (at >= key.size() || key[at] < '0' || key[at] > '9') return false;
    std::uint64_t value = 0;
    while (at < key.size() && key[at] >= '0' && key[at] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(key[at] - '0');
        if (value > 1000000ull) return false;
        ++at;
    }
    if (key.compare(at, middle.size(), middle) != 0) return false;
    version = key.substr(at + middle.size());
    if (version.empty()) return false;
    slot = static_cast<int>(value);
    return true;
}

namespace {

// Every id of the list must name the morph table entry it claims.
bool TracksMatchModel(const unsigned char* model, const std::vector<ac::NamedTrack>& tracks) {
    if (tracks.empty()) return false;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    std::set<int> seen;
    for (const ac::NamedTrack& named : tracks) {
        int morph = -1;
        std::string error;
        if (!TrackMatchesModel(record, named, morph, error)) return false;
        if (!seen.insert(morph).second) return false;
    }
    return true;
}

// The PMM record carries the effective curve: the standard PMM body holds one
// key per native record regardless of the export mark, so the saved curve is
// exactly what played.
std::vector<ac::NamedTrack> AbsoluteCurves(const unsigned char* model, const State& state) {
    std::vector<ac::NamedTrack> tracks;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const mdl::MorphRecord* const morphs = record.morphs;
    if (morphs == nullptr) return tracks;
    tracks.reserve(state.effective.size());
    for (const auto& entry : state.effective) {
        const int morph = entry.first;
        if (morph < 0 || static_cast<std::uint32_t>(morph) >= record.morphCount) continue;
        if (entry.second.keys.empty()) continue;
        ac::NamedTrack named;
        named.id = StableId(morph, morphs[morph].name);
        named.track = entry.second;
        tracks.push_back(std::move(named));
    }
    return tracks;
}

}  // namespace

void CapturePmm(const MMDApp& app, pmm_extension::Document& document) {
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        const std::string key = PmmKey(slot);
        std::vector<std::string> unknown;
        for (const pmm_extension::Record& entry : document.records) {
            int other = -1;
            std::string version;
            if (!ParsePmmKey(entry.key, other, version) || other != slot) continue;
            if (version != kPmmVersion) unknown.push_back(entry.key);
        }
        unsigned char* model = app.ModelSlot(slot);
        if (model == nullptr) {
            // The slot is empty now: nothing may inherit what a previous model
            // of this slot left in the project.
            for (const std::string& name : unknown) pmm_extension::Erase(document, name);
            pmm_extension::Erase(document, key);
            continue;
        }
        // Synced first, so the project stores what is playing right now rather
        // than the curve as it stood at the last evaluation.
        State* const state = Current(model);
        if (state != nullptr) {
            if (state->effective.empty()) {
                for (const std::string& name : unknown) pmm_extension::Erase(document, name);
                pmm_extension::Erase(document, key);
                continue;
            }
            std::vector<std::uint8_t> value;
            std::string error;
            const std::vector<ac::NamedTrack> tracks = AbsoluteCurves(model, *state);
            if (tracks.empty() || !EncodePayloadImpl(tracks, value, error)) {
                runtime_log::Write("morph curve: model slot %d curves cannot be encoded (%s)", slot,
                                   error.c_str());
                continue;
            }
            pmm_extension::Set(document, key, std::move(value));
            continue;
        }
        // No authoring in this session.  Keep the record only while every track
        // still describes the model that sits in the slot; a record for another
        // model must not be handed over as this model's own curves.
        const pmm_extension::Record* record = pmm_extension::Find(document, key);
        if (record == nullptr) continue;
        std::vector<ac::NamedTrack> stored;
        std::string error;
        if (DecodePayloadImpl(record->value.data(), record->value.size(), stored, error) &&
            TracksMatchModel(model, stored)) {
            continue;
        }
        for (const std::string& name : unknown) pmm_extension::Erase(document, name);
        pmm_extension::Erase(document, key);
    }
}

void LoadPmm(const MMDApp& app, const pmm_extension::Document& document) {
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        unsigned char* model = app.ModelSlot(slot);
        if (model == nullptr) continue;
        const pmm_extension::Record* record = pmm_extension::Find(document, PmmKey(slot));
        if (record == nullptr) continue;
        std::vector<ac::NamedTrack> tracks;
        std::string error;
        if (!DecodePayloadImpl(record->value.data(), record->value.size(), tracks, error)) {
            runtime_log::Write(
                "morph curve: PMM record of model slot %d is malformed and was left in the "
                "project unchanged (%s)",
                slot, error.c_str());
            continue;
        }
        bool valid = !tracks.empty() && TracksMatchModel(model, tracks);
        if (valid) {
            for (const ac::NamedTrack& named : tracks) {
                if (!TrackInvariants(named, error)) {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            runtime_log::Write(
                "morph curve: PMM record of model slot %d does not fit the loaded model and was "
                "left in the project unchanged",
                slot);
            continue;
        }
        State* state = Lookup(model);
        if (state == nullptr) {
            State fresh;
            fresh.identity = IdentityOf(model);
            state = &States().emplace(model, std::move(fresh)).first->second;
        }
        for (const ac::NamedTrack& named : tracks) {
            int morph = -1;
            std::string name;
            ParseStableId(named.id, morph, name);
            std::vector<NativeKey> native;
            std::string reason;
            if (morph < 0 || !ReadChain(mdl::Mdl(model)->morphKeys, morph, native, reason)) continue;
            // The project record is the complete curve of that morph: it becomes
            // the pristine layer, mirrored by the chain the standard body loaded.
            SetInstalled(*state, morph, named.track, native);
        }
    }
}

// ---------------------------------------------------------------------------
// Model VMD block
// ---------------------------------------------------------------------------

namespace {

// The keys of one effective curve that the standard morph section carries:
// exactly the frames of the allocated native records (the same "selected" mark
// vmd_save writes), rebased by minFrame.  The authored interpolation of a key
// survives only while the key before it in the exported track is also the key
// before it in the effective curve; a key whose neighbour was not exported, or
// the first key of the track, opens an interval whose authored ease no longer
// describes its span and goes back to the native interpolation.  Keys that are
// not selected are simply left out - the exporter never invents a key that the
// standard section does not have.
bool SelectExported(const ac::Track& effective, const std::vector<NativeKey>& chain,
                    std::uint32_t minFrame, ac::Track& out) {
    out = ac::Track{};
    bool previousKept = false;
    for (const ac::Key& key : effective.keys) {
        const NativeKey* native = FindNative(chain, key.frame);
        const bool selected = native != nullptr && native->allocated &&
                              key.frame >= static_cast<std::int32_t>(minFrame);
        if (!selected) {
            previousKept = false;
            continue;
        }
        ac::Key copy = key;
        if (!previousKept) MakeNativeInterpolation(copy);
        copy.frame -= static_cast<std::int32_t>(minFrame);
        out.keys.push_back(copy);
        previousKept = true;
    }
    return !out.keys.empty();
}

// Merges one imported curve (already in model frames) into the target track.
// Every target key outside the imported set survives, and so does a target key
// the import does not name; the imported keys win at their own frames with their
// own authored interpolation.  A target interval an imported key splits loses
// its authored ease, because that ease was calibrated for the old span.
void MergeImported(const ac::Track& target, const ac::Track& imported, ac::Track& out) {
    out = target;
    const auto compare = [](const ac::Key& key, std::int32_t frame) { return key.frame < frame; };
    std::set<std::int32_t> importedFrames;
    for (const ac::Key& key : imported.keys) importedFrames.insert(key.frame);
    std::set<std::int32_t> invalidated;
    for (std::size_t index = 0; index < imported.keys.size(); ++index) {
        ac::Key key = imported.keys[index];
        // The first imported key governs no interval of its own, so it must not
        // fabricate one over whatever target key precedes it.
        if (index == 0) MakeNativeInterpolation(key);
        auto at = std::lower_bound(out.keys.begin(), out.keys.end(), key.frame, compare);
        if (at != out.keys.end() && at->frame == key.frame) {
            *at = key;
            continue;
        }
        if (at != out.keys.end()) invalidated.insert(at->frame);
        out.keys.insert(at, key);
    }
    for (ac::Key& key : out.keys) {
        if (invalidated.count(key.frame) != 0 && importedFrames.count(key.frame) == 0) {
            MakeNativeInterpolation(key);
        }
    }
    // An imported interval keeps its authored ease only while its two keys are
    // still neighbours: a target key sitting between them makes the authored
    // span wrong, so that end goes back to the native interpolation.
    for (std::size_t index = 1; index < imported.keys.size(); ++index) {
        auto at = std::lower_bound(out.keys.begin(), out.keys.end(), imported.keys[index].frame,
                                   compare);
        if (at == out.keys.end() || at->frame != imported.keys[index].frame) continue;
        if (at == out.keys.begin() || (at - 1)->frame != imported.keys[index - 1].frame) {
            MakeNativeInterpolation(*at);
        }
    }
    ac::UpdateAutomatic(out);
}

}  // namespace

bool CaptureVmd(const unsigned char* model, std::uint32_t minFrame, std::vector<std::uint8_t>& out,
                std::string& error) {
    out.clear();
    error.clear();
    // Synced first, so the block carries what is playing right now.
    const State* const state = Current(model);
    if (state == nullptr || state->effective.empty()) return true;
    // vmd_save passes 0xFFFFFFFF when no key is exported at all, and a frame
    // cannot be rebased onto an out-of-range minimum.
    if (minFrame > static_cast<std::uint32_t>(kMaximumFrame)) return true;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const mdl::MorphRecord* const morphs = record.morphs;
    const mdl::MorphKey* keys = record.morphKeys;
    if (morphs == nullptr || keys == nullptr) return true;

    std::vector<ac::NamedTrack> tracks;
    for (const auto& entry : state->effective) {
        const int morph = entry.first;
        if (morph < 0 || static_cast<std::uint32_t>(morph) >= record.morphCount) continue;
        if (entry.second.keys.empty()) continue;
        // The exported set is read fresh from the pool: the standard morph
        // section is written from the same marks, and vmd_save derives minFrame
        // from them, so every selected frame is >= minFrame by construction.  A
        // curve whose keys are all unselected exports nothing at all - and a
        // curve whose first keys are unselected exports the selected ones
        // instead of disappearing whole.
        std::vector<NativeKey> chain;
        std::string reason;
        if (!ReadChain(keys, morph, chain, reason)) continue;
        ac::Track exported;
        if (!SelectExported(entry.second, chain, minFrame, exported)) continue;
        ac::NamedTrack named;
        named.id = StableId(morph, morphs[morph].name);
        named.track = std::move(exported);
        tracks.push_back(std::move(named));
    }
    if (tracks.empty()) return true;

    std::vector<std::uint8_t> payload;
    if (!EncodePayloadImpl(tracks, payload, error)) return false;
    return EncodeVmdBlock(payload, out, error);
}

std::size_t VmdBlockSize(const std::uint8_t* data, std::size_t size) {
    return VmdBlockSizeImpl(data, size);
}

std::size_t DecodeVmdBlock(const std::uint8_t* data, std::size_t size,
                           std::vector<std::uint8_t>& payload) {
    payload.clear();
    const std::size_t total = VmdBlockSizeImpl(data, size);
    // Decoding needs the whole block, not just the header a probe may have read.
    if (total == 0 || total > size) return 0;
    const std::size_t bodyBytes = total - kVmdBlockHeaderBytes;
    if (bodyBytes > kMaximumPayloadBytes) return 0;
    std::size_t at = sizeof(kVmdBlockMagic);
    std::uint32_t declared = 0;
    std::uint32_t stored = 0;
    GetU32(data, size, at, declared);
    GetU32(data, size, at, stored);
    if (stored != Crc32(data + kVmdBlockHeaderBytes, bodyBytes)) return 0;
    payload.assign(data + kVmdBlockHeaderBytes, data + total);
    return total;
}

bool EncodeVmdBlock(const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& out,
                    std::string& error) {
    out.clear();
    error.clear();
    if (payload.empty()) {
        error = "there is no morph curve payload to write";
        return false;
    }
    if (payload.size() > kMaximumPayloadBytes) {
        error = "the morph curve block would need " + std::to_string(payload.size()) +
                " bytes, the limit is " + std::to_string(kMaximumPayloadBytes);
        return false;
    }
    out.reserve(kVmdBlockHeaderBytes + payload.size());
    out.insert(out.end(), kVmdBlockMagic, kVmdBlockMagic + sizeof(kVmdBlockMagic));
    PutU32(out, static_cast<std::uint32_t>(kVmdBlockHeaderBytes + payload.size()));
    PutU32(out, Crc32(payload.data(), payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

bool EncodePayload(const std::vector<ac::NamedTrack>& tracks, std::vector<std::uint8_t>& out,
                   std::string& error) {
    return EncodePayloadImpl(tracks, out, error);
}

bool DecodePayload(const std::uint8_t* data, std::size_t size,
                   std::vector<ac::NamedTrack>& out, std::string& error) {
    return DecodePayloadImpl(data, size, out, error);
}

std::size_t ApplyVmd(unsigned char* model, const std::uint8_t* data, std::size_t size,
                     int frameOffset) {
    if (model == nullptr || data == nullptr || size == 0) return 0;
    std::vector<std::uint8_t> payload;
    if (DecodeVmdBlock(data, size, payload) == 0) return 0;
    std::vector<ac::NamedTrack> tracks;
    std::string error;
    if (!DecodePayloadImpl(payload.data(), payload.size(), tracks, error)) return 0;

    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const mdl::MorphRecord* const morphs = record.morphs;
    const mdl::MorphKey* keys = record.morphKeys;
    if (morphs == nullptr || keys == nullptr || record.morphCount == 0) return 0;

    std::size_t applied = 0;
    std::size_t skipped = 0;
    for (const ac::NamedTrack& named : tracks) {
        int claimed = -1;
        std::string name;
        if (!ParseStableId(named.id, claimed, name) || name.size() != kNameBytes ||
            named.track.keys.empty()) {
            ++skipped;
            continue;
        }
        // 1. The standard importer matches morphs by name, so this does too; an
        //    ambiguous name is skipped rather than guessed.
        int match = -1;
        int matches = 0;
        if (const auto* targets=extended_names::importedMorphTargets) {
            if (claimed>=0 && std::size_t(claimed)<targets->size()) match=(*targets)[claimed];
            matches=match>=0 && std::uint32_t(match)<record.morphCount ? 1 : 0;
        } else {
        for (std::uint32_t index = 0; index < record.morphCount; ++index) {
            if (std::memcmp(name.data(), morphs[index].name, kNameBytes) == 0) {
                match = static_cast<int>(index);
                ++matches;
            }
        }
        }
        if (matches != 1) {
            ++skipped;
            continue;
        }
        std::vector<NativeKey> chain;
        std::string reason;
        if (!ReadChain(keys, match, chain, reason)) {
            ++skipped;
            continue;
        }
        // 2. Every key of the block must be a key this very import created: the
        //    frame has to carry an allocated native record and the value has to
        //    agree at the precision the standard record stores.  A block that
        //    does not describe the import is refused as a whole, so it can never
        //    rewrite frames it did not come with.
        ac::Track imported = named.track;
        bool describesImport = true;
        for (ac::Key& key : imported.keys) {
            const std::int64_t shifted = static_cast<std::int64_t>(key.frame) + frameOffset;
            if (shifted < 0 || shifted > kMaximumFrame) {
                describesImport = false;
                break;
            }
            key.frame = static_cast<std::int32_t>(shifted);
            const NativeKey* native = FindNative(chain, key.frame);
            if (native == nullptr || !native->allocated ||
                static_cast<float>(key.value) != static_cast<float>(native->value)) {
                describesImport = false;
                break;
            }
        }
        if (!describesImport) {
            ++skipped;
            continue;
        }
        // 3. Merge into the target's own curve.  A morph without authoring yet
        //    starts from the native track it already had, so the imported curve
        //    is layered onto real keys instead of replacing them.  The state is
        //    derived first, so a native edit that happened before this import -
        //    and the keys the import itself just registered - are part of the
        //    base rather than being folded into the new mirror.
        State* state = Current(model);
        ac::Track target;
        if (state != nullptr) {
            const auto found = state->effective.find(match);
            if (found != state->effective.end()) target = found->second;
        }
        if (target.keys.empty()) target = CurveOf(chain);
        ac::Track merged;
        MergeImported(target, imported, merged);
        // 4. The frame-zero anchor is the target's own value at frame zero, never
        //    a copy of the imported first key: a motion imported at an offset
        //    must not move the pose the model already had at frame zero.
        if (!merged.keys.empty() && merged.keys.front().frame > 0) {
            if (keys[match].frame != 0 || !std::isfinite(keys[match].value)) {
                ++skipped;
                continue;
            }
            ac::Key head;
            head.frame = 0;
            head.value = static_cast<double>(keys[match].value);
            MakeNativeInterpolation(head);
            merged.keys.insert(merged.keys.begin(), head);
        }
        // 5. Everything is validated before a single key is installed.
        if (!TrackInvariants(ac::NamedTrack{StableId(match, morphs[match].name), merged}, reason)) {
            ++skipped;
            continue;
        }
        if (state == nullptr) {
            State fresh;
            fresh.identity = IdentityOf(model);
            state = &States().emplace(model, std::move(fresh)).first->second;
        }
        // The chain as it stands now is the shape this curve mirrors.
        SetInstalled(*state, match, merged, chain);
        ++applied;
    }
    if (skipped > 0) {
        runtime_log::Write(
            "morph curve: VMD block applied %llu of %llu tracks (name, imported key or capacity "
            "mismatch skipped)",
            static_cast<unsigned long long>(applied), static_cast<unsigned long long>(tracks.size()));
    }
    return applied;
}

// ---------------------------------------------------------------------------
// Native frame retime support (see morph_curve.hpp)
// ---------------------------------------------------------------------------
//
// The authored layers are exchanged as one object, exactly like the bone scale
// curve table: CaptureLayers copies the live layers, RebuildLayers builds the
// replacement from the retimed pristine curves against the model's own retimed
// pool, and SwapLayers exchanges live and holder.  Installing is one swap and
// rolling a commit back is the same swap called again, so neither allocates.

struct AuthoredLayers::Impl {
    bool present = false;
    const unsigned char* model = nullptr;
    State state;
};

AuthoredLayers::AuthoredLayers() : impl_(new Impl()) {}
AuthoredLayers::~AuthoredLayers() = default;
AuthoredLayers::AuthoredLayers(AuthoredLayers&&) noexcept = default;
AuthoredLayers& AuthoredLayers::operator=(AuthoredLayers&&) noexcept = default;

namespace {

void MixTrack(std::uint64_t& hash, const ac::Track& track) {
    const auto mix = [&hash](std::uint64_t value) { hash ^= value; hash *= 1099511628211ull; };
    mix(track.discrete ? 1u : 0u);
    for (const ac::Key& key : track.keys) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.frame)));
        std::uint64_t bits = 0;
        std::memcpy(&bits, &key.value, sizeof(bits));
        mix(bits);
        std::memcpy(&bits, &key.inSlope, sizeof(bits));
        mix(bits);
        std::memcpy(&bits, &key.outSlope, sizeof(bits));
        mix(bits);
        std::memcpy(&bits, &key.inWeight, sizeof(bits));
        mix(bits);
        std::memcpy(&bits, &key.outWeight, sizeof(bits));
        mix(bits);
        mix(static_cast<std::uint64_t>(key.interpolation));
        mix(static_cast<std::uint64_t>(key.tangent));
    }
}

void MixChain(std::uint64_t& hash, const std::vector<NativeKey>& chain) {
    const auto mix = [&hash](std::uint64_t value) { hash ^= value; hash *= 1099511628211ull; };
    for (const NativeKey& key : chain) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.frame)));
        std::uint64_t bits = 0;
        std::memcpy(&bits, &key.value, sizeof(bits));
        mix(bits);
        mix(key.allocated ? 1u : 0u);
    }
}

std::uint64_t LayersDigest(const State& state) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) { hash ^= value; hash *= 1099511628211ull; };
    for (const auto& entry : state.authored) {
        mix(static_cast<std::uint64_t>(entry.first));
        MixTrack(hash, entry.second);
    }
    for (const auto& entry : state.effective) {
        mix(static_cast<std::uint64_t>(entry.first));
        MixTrack(hash, entry.second);
    }
    for (const auto& entry : state.mirror) {
        mix(static_cast<std::uint64_t>(entry.first));
        MixChain(hash, entry.second);
    }
    for (const auto& entry : state.observed) {
        mix(static_cast<std::uint64_t>(entry.first));
        MixChain(hash, entry.second);
    }
    return hash;
}

}  // namespace

bool CaptureLayers(const unsigned char* model, AuthoredLayers& out, std::string& error) {
    error.clear();
    if (!out.impl_) out.impl_.reset(new AuthoredLayers::Impl());
    out.impl_->present = false;
    out.impl_->model = model;
    out.impl_->state = State{};
    if (model == nullptr) return true;
    // A stale entry (a recycled address, a reloaded model) is not this model's
    // authoring, and this call must not touch the module to find that out.
    const auto found = States().find(model);
    if (found == States().end() || !Matches(found->second.identity, model)) return true;
    if (found->second.authored.empty()) return true;
    out.impl_->present = true;
    out.impl_->state = found->second;
    return true;
}

bool LayersTracks(const AuthoredLayers& layers, std::vector<ac::NamedTrack>& out, std::string& error) {
    out.clear();
    error.clear();
    if (!layers.impl_ || !layers.impl_->present) return true;
    const unsigned char* model = layers.impl_->model;
    if (model == nullptr) return true;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const mdl::MorphRecord* morphs = record.morphs;
    if (morphs == nullptr) return true;
    // Derive on a copy: native edits since the last read must be included, while
    // the live authoring state remains byte-for-byte unchanged during preview.
    State effective = layers.impl_->state;
    Sync(model, effective);
    for (const auto& entry : effective.effective) {
        if (entry.first < 0 || static_cast<std::uint32_t>(entry.first) >= record.morphCount) continue;
        ac::NamedTrack named;
        named.id = StableId(entry.first, morphs[entry.first].name);
        named.track = entry.second;
        out.push_back(std::move(named));
    }
    return true;
}

bool RebuildLayers(const unsigned char* model, const std::vector<ac::NamedTrack>& authored,
                   AuthoredLayers& out, std::string& error) {
    error.clear();
    if (!out.impl_) out.impl_.reset(new AuthoredLayers::Impl());
    out.impl_->present = false;
    out.impl_->model = model;
    out.impl_->state = State{};
    if (model == nullptr) return true;
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const mdl::MorphRecord* morphs = record.morphs;
    if (morphs == nullptr) return true;
    State built;
    built.identity = IdentityOf(model);
    for (const ac::NamedTrack& named : authored) {
        int morph = -1;
        std::string name;
        if (!ParseStableId(named.id, morph, name)) {
            error = "a retimed expression track is not a morph identity";
            return false;
        }
        if (morph < 0 || static_cast<std::uint32_t>(morph) >= record.morphCount ||
            StableId(morph, morphs[morph].name) != named.id) {
            error = "a retimed expression track does not name a morph of this model";
            return false;
        }
        std::string detail;
        if (!TrackInvariants(named, detail)) {
            error = detail;
            return false;
        }
        // The mirror and the observed chain are deliberately left out.  Sync()
        // treats a state without them as "the chain I see now is the one this
        // curve was installed with", so the next access takes the retimed pool
        // as the installed shape and the retimed ease stays in force.  That is
        // what lets this run at Prepare time, before a single byte is written,
        // and keeps Commit free of the allocation this used to do.
        built.authored[morph] = named.track;
        built.effective[morph] = named.track;
    }
    out.impl_->present = !built.authored.empty();
    out.impl_->state = std::move(built);
    return true;
}

void SwapLayers(unsigned char* model, AuthoredLayers& layers) noexcept {
    if (model == nullptr || !layers.impl_) return;
    const auto found = States().find(model);
    // The holder always comes from an entry this module created and nothing
    // removes it while a plan is alive, so a miss leaves the module alone
    // instead of inserting (which would have to allocate).
    if (found == States().end()) return;
    std::swap(found->second, layers.impl_->state);
    layers.impl_->model = model;
}

bool LayersUnchanged(const unsigned char* model, const AuthoredLayers& layers) {
    if (!layers.impl_) return false;
    if (!layers.impl_->present) {
        const auto found = States().find(model);
        return found == States().end() || !Matches(found->second.identity, model) || found->second.authored.empty();
    }
    if (model == nullptr || layers.impl_->model != model) return false;
    const auto found = States().find(model);
    if (found == States().end()) return false;
    if (!Matches(found->second.identity, model)) return false;
    return LayersDigest(found->second) == LayersDigest(layers.impl_->state);
}

}  // namespace mikudancestudio::morph_curve
