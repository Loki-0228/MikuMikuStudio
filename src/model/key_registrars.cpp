#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/key_pool.hpp"
// ===========================================================================
// VA 0x0049D880 / 0x0049E310 / 0x0049F190 / 0x0049F8C0 /
//    0x004A4940 / 0x004A49A0 / 0x004A27F0
// - key registrars + allocator resets for the VMD load path, plus the
//   name-based key-track frame marker of the frame-range editor
// ===========================================================================
// The three registrars insert one key each into the per-track sorted
// doubly-linked lists inside the shared record arrays, allocating fresh
// records from the model+8768 cursor (advanced past frame==0 holes; the
// cursor increments persist):
//
//   RegisterBoneKey(model, rec, frameOffset, useSelected)   bone keys
//     - rec = the 0x434B60 parse buffer: name at +0, frame +32, position
//       +52..60, quaternion +36..52 (quaternion w at +52 = v66[56]...
//       note the DECOMPILE-SHIFTED naming below uses the byte offsets the
//       loader wrote), control block +64..80 (the 3939-compare byte at
//       +64 becomes the record's IK-off mode byte at +57; +65..80 copy
//       linearly to the record's +12..27).
//     - frame = frameOffset + rec.frame (VMD load passes the CURRENT
//       frame, so a motion loads "starting at" the play position).
//     - bone lookup: useSelected ? model+11664 selected index : strcmp
//       over the typed BoneRecord table (name); miss -> 1.
//     - twist correction for type 8 / type 4 with the 0x400 flag at
//       bone+500: quaternion -> axis/angle, axis forced onto the bone's
//       own axis (PMX: bone+508..516; PMD: normalize(tail rest position
//       - bone rest position), tail index at bone+460); cos = dot of the
//       normalized axes, negative -> negate sin(halfAngle); xyz rebuilt
//       and normalized.
//     - position is stored only for bone types 0/1/2.
//     - insert: walk next-links from the bone index, exact frame =
//       overwrite in place, else allocate + splice (prev/next), append
//       when the chain ends.  The original overflows at kBoneKeyCapacity
//       records (the x64 E build's 600000; the x86 original's 300000) and
//       raises the "You cannot regist over %d point" box (EN/JP by
//       model+12740).  This port grows the pool on demand and raises that
//       box only when the pool cannot grow - the structural index ceiling,
//       or an allocation failure that leaves the model untouched after a
//       transaction.  model+12720 max-frame bump (exact/insert paths bump
//       the RAW frameOffset - an original quirk kept).
//     - AppendBoneKeyToUndo (0x49D410; was Sub49D410 - undo-slot
//       bookkeeping, deduped by the model keyVisitMap) fires per touched
//       record.
//
//   RegisterMorphKeyFromRecord (was Sub49F190) (model, rec, frameOffset)
//     morph keys - same walk/insert over the 20-byte records; value at
//     rec+36, mark +16; cap 20000.
//
//   RegisterDisplayKeyFromRecord (was Sub49F8C0) (model, frame, view, cnt,
//             entries21, cnt2, entries28, frameOffset)   IK/display keys
//     into the 28-byte master records (single chain from record 0): +12
//     view byte, mark +20, cap 1000.  entries21 = {20-byte name, flag}
//     matched per IK chain (chain bone indices are the first dwords of the
//     model+9920 24-byte structs) filling the record+16 byte array;
//     entries28 = {20-byte name, u32, u32} matched per selector bone
//     (first dword of each 20-byte selector record) filling the record+24
//     {frame, idx} pair array.  VMD load passes cnt2 = 0.
//
//   ResetBoneKeyCursor(model)  resets model+8768 to the bone count, then skips
//     occupied (frame != 0) 60-byte records (cap = the pool's runtime
//     capacity; this reset never grows the pool).
//   ResetMorphKeyCursor(model)  reseeds model+8768 to the morph count and skips
//     occupied 20-byte records (cap 20000).
//
//   MarkKeyTrackRangeByName (was Sub4A27F0) (model, from, to, name)
//     name-based key-track frame mark:
//     resolves the scope text of the frame-range editor (command 415:
//     the 0x1B2 text / the "Sel Bone" and "Sel facial" per-item loops)
//     to ONE key track and sets the mark byte (+56 bone / +16 morph /
//     +20 display) of every record with from <= frame <= to (unsigned).
//     Resolution order (first match wins, misses fall through):
//       1. facial display-frame group name (displayFrames, SJIS name
//          ONLY - an English-mode group text never matches, original
//          quirk) -> morph-key track rooted at targetIndex;
//       2. 表示･IK･外親 (13-byte memcmp) / "disp/IK/OP" -> display-key
//          track (record 0);
//       3. the root bone's own name, bones[0] SJIS or English (combo
//          434's center-bone entry) -> bone-key record 0;
//       4. ﾎﾞｰﾝ01 (7-byte memcmp) / "bone01" - the bundled ダミーボーン
//          model's combo entries (0x531118/0x531110, x64 0x5524D8/
//          0x5524E0) -> bone-key record 0;
//       5. bone display-frame group name (rbGroups, SJIS or English) ->
//          bone-key track rooted at targetIndex.
//     Callers ignore the return value; it carries the last record index
//     marked (0 = nothing), mirroring the original's leftover eax.
//
// Reference: IDA live disassembly of MikuMikuDance.exe v932 (sole source of
// truth; ../translated/ reference files deviate).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/text_encoding.hpp"

#include "keyframe_common.hpp"

namespace mikumikustudio {
namespace {

using kfa::Rd32;
using kfa::RdI32;
using kfa::RdF32;
using kfa::Wr32;
using kfa::WrF32;
using kfa::RdPtr;

// JP overflow strings (0x52B918 / 0x52B908, SJIS byte-exact)
const char kJpOverflow[] =
    "\x93\x6f\x98\x5e\x83\x7c\x83\x43\x83\x93\x83\x67\x90\x94\x82\xaa%d"
    "\x8c\xc2\x82\xf0\x89\x7a\x82\xa6\x82\xdc\x82\xb5\x82\xbd\n"
    "\x82\xb1\x82\xea\x88\xc8\x8f\xe3\x82\xcc\x93\x6f\x98\x5e\x82\xcd\x8d"
    "\x73\x82\xa6\x82\xdc\x82\xb9\x82\xf1\n"
    "\x81\x75\xcc\xda\xb0\xd1\x95\xd2\x8f\x57\x81\x76\x82\xcc\x81\x75\x95"
    "\x73\x97\x70\xcc\xda\xb0\xd1\x8d\xed\x8f\x9c\x81\x76\x82\xf0\x8e\xc0"
    "\x8d\x73\x82\xb5\x82\xc4\x89\xba\x82\xb3\x82\xa2";
const char kJpFrameRegTitle[] =  // 0x52B908 "フレーム登録"
    "\xcc\xda\xb0\xd1\x93\x6f\x98\x5e";

// shared overflow message (limit differs per registrar); the box owner
// HWND is read from model+0
void OverflowBox(unsigned char* m, int limit) {
    char text[256];
    HWND owner = *reinterpret_cast<HWND*>(m);
    // x64 sub_7FF7CB4E9390 @0x7FF7CB4E9D4B: language byte selects both the
    // format and the caption ("register frame" / フレーム登録).  Both bodies go
    // through MessageBoxJp: the JP literals are byte-exact Shift-JIS, and
    // MessageBoxA decodes them with the *system* code page (CP936 on a Chinese
    // Windows), which garbles every line of this box - the box the F-key
    // "paste to another frame" command (case 250) can raise from
    // RegisterBoneKey.  ASCII decodes identically under CP932, so the English
    // branch is unaffected.
    if (mikumikustudio::mdl::Mdl(m)->physicsFlags != 0) {
        sprintf_s(text, 0x100,
                  "You cannot regist over %d point\n"
                  "Please execute 'delete unused frame'", limit);
        text_encoding::MessageBoxJp(owner, text, "register frame", 0);
    } else {
        sprintf_s(text, 0x100, kJpOverflow, limit);
        text_encoding::MessageBoxJp(owner, text, kJpFrameRegTitle, 0);
    }
}

// These pools now grow on demand. Allocation/index failure must not
// claim that the old initial capacity is a registration policy limit.
void KeyPoolAllocationBox(unsigned char* model) {
    const unsigned language = g_Block ? g_Block->EnglishUI() : 1;
    const wchar_t* text[] = {
        L"キーフレーム用のメモリを確保できないか、インデックス範囲を超えました。",
        L"Cannot allocate keyframe memory or represent more key indices.",
        L"无法分配关键帧内存，或关键帧索引超出可表示范围。"};
    const wchar_t* title[] = {L"フレーム登録", L"register frame", L"注册关键帧"};
    MessageBoxW(*reinterpret_cast<HWND*>(model), text[language <= 2 ? language : 1],
                title[language <= 2 ? language : 1], MB_OK | MB_ICONERROR);
}

// Grows the bone-key pool when a free-slot scan has reached the end of the
// current block and the caller still needs one more slot.  Returns the new pool
// pointer, or nullptr when the pool cannot grow (the structural ceiling or a
// failed allocation); the caller then reports the overflow exactly as before.
// Every caller must refresh its cached pool pointer and capacity from this
// result and from mdl::BoneKeyCapacity.
mdl::BoneKey* GrowBoneKeyPool(unsigned char* m) {
    mdl::ModelRecord& model = *mdl::Mdl(m);
    const std::uint64_t needed =
        static_cast<std::uint64_t>(mdl::BoneKeyCapacity(model.boneKeys)) + 1;
    if (!mdl::BoneKeyPoolEnsure(model, needed)) return nullptr;
    return model.boneKeys;
}

// Finds a free bone-key slot for an insertion.  The scan starts at the
// persistent search cursor and walks past occupied records; when it reaches the
// end of the current block the pool grows and the scan continues in the fresh,
// zero-filled region.  Returns the slot, or -1 when the pool cannot grow any
// further (out of memory or at the structural ceiling); the caller then reports
// the overflow exactly as it did when the scan hit the fixed pool end.
//
// Growth moves the pool, so `keys` and `capacity` are refreshed here; callers
// must index the pool again through the refreshed pointer rather than keep a
// reference taken before the call.
int AllocateBoneKeySlot(unsigned char* m, mdl::BoneKey*& keys,
                        std::size_t& capacity) {
    int slot = mdl::Mdl(m)->searchCursor;
    if (static_cast<std::size_t>(slot) >= capacity) {
        keys = GrowBoneKeyPool(m);
        if (keys == nullptr) return -1;
        capacity = mdl::BoneKeyCapacity(*mdl::Mdl(m));
    }
    if (keys[slot].frame == 0) return slot;
    for (;;) {
        ++mdl::Mdl(m)->searchCursor;
        ++slot;
        if (static_cast<std::size_t>(slot) >= capacity) {
            keys = GrowBoneKeyPool(m);
            if (keys == nullptr) return -1;
            capacity = mdl::BoneKeyCapacity(*mdl::Mdl(m));
        }
        if (keys[slot].frame == 0) return slot;
    }
}

// copy the parsed bone-key fields into a 60-byte record (the exact-
// overwrite and fresh-record paths share this; dst frame/links excluded)
void FillBoneRecord(mdl::BoneKey& dst, const unsigned char* rec,
                    bool writePos) {
    std::memcpy(dst.rotation, rec + 36, sizeof dst.rotation);
    if (writePos) std::memcpy(dst.position, rec + 52, sizeof dst.position);
    dst.physicsDisabled = rec[64];
    std::memcpy(dst.interpolation, rec + 65, sizeof dst.interpolation);
    dst.allocated = 1;
}

// 0x49E310 stores the copied pose after reflecting it across the model's
// YZ plane. Quaternion Y/Z and position X change sign.
void FillMirroredBoneRecord(mdl::BoneKey& dst, const unsigned char* rec) {
    std::memcpy(dst.rotation, rec + 36, sizeof dst.rotation);
    dst.rotation[1] = -dst.rotation[1];
    dst.rotation[2] = -dst.rotation[2];
    std::memcpy(dst.position, rec + 52, sizeof dst.position);
    dst.position[0] = -dst.position[0];
    dst.physicsDisabled = rec[64];
    std::memcpy(dst.interpolation, rec + 65, sizeof dst.interpolation);
    dst.allocated = 1;
}

int FindMirroredBone(unsigned char* model, const unsigned char* rec) {
    // Shift-JIS: 0x89 0x45 = 右, 0x8D 0xB6 = 左. MMD swaps the first
    // matching marker and compares the suffix after the two-byte character.
    static const char kRight[] = "\x89\x45";
    static const char kLeft[] = "\x8D\xB6";
    const char* const name = reinterpret_cast<const char*>(rec);
    const mdl::ModelRecord& state = *mdl::Mdl(model);
    const mdl::BoneRecord* const bones = state.boneTable;
    const int boneCount = static_cast<int>(state.boneCount);

    const auto findOpposite = [&](const char* marker,
                                  const char* opposite) -> int {
        const char* hit = std::strstr(name, marker);
        if (hit == nullptr || boneCount <= 0) return -1;
        const char* const suffix = hit + 2;
        for (int i = 0; i < boneCount; ++i) {
            const char* const boneName = bones[i].name;
            const char* other = std::strstr(boneName, opposite);
            if (other != nullptr && std::strcmp(suffix, other + 2) == 0)
                return i;
        }
        return -1;
    };

    // x64 sub_7FF7CB4E9DB0: first 左 in the source name (0x7FF7CB4E9DE6,
    // strstr @0x7FF7CB4E9DF7) -> bones containing 右; only when the name
    // lacks 左 does the 右-in-source -> 左-in-bones pass run (0x7FF7CB4E9E7C).
    int index = findOpposite(kLeft, kRight);
    if (index >= 0) return index;
    index = findOpposite(kRight, kLeft);
    if (index >= 0) return index;
    for (int i = 0; i < boneCount; ++i) {
        if (std::strcmp(name, bones[i].name) == 0)
            return i;
    }
    return -1;
}

// Name probes of Sub4A27F0 (SJIS byte-exact, compare lengths as in the
// binary, NUL included; x86 0x531120 / 0x531118 = x64 0x552048 /
// 0x5524D8).
const unsigned char kJpDispIkOp[] =   // 表示･IK･外親 (13)
    {0x95, 0x5C, 0x8E, 0xA6, 0xA5, 0x49, 0x4B,
     0xA5, 0x8A, 0x4F, 0x90, 0x65, 0x00};
const unsigned char kJpBone01[] =     // ﾎﾞｰﾝ01 (7)
    {0xCE, 0xDE, 0xB0, 0xDD, 0x30, 0x31, 0x00};

// Track walk + mark shared by every branch of Sub4A27F0: `root` is the
// track's head record (record i of the bone/morph arrays, 0 for the
// display array).  Advance while frame < from (a chain that ends before
// `from` marks nothing - 0x4A2B15/0x4A2955/0x4A28B5), then set the mark
// byte of every record with frame <= to.  Returns the last record index
// marked, 0 when nothing was in range.
template <typename Key>
int MarkTrackRange(Key* keys, int root, std::uint32_t from,
                   std::uint32_t to) {
    int cur = root;
    if (keys[cur].frame < from) {
        for (;;) {
            const int next = static_cast<int>(keys[cur].next);
            if (next == 0) return 0;
            cur = next;
            if (keys[cur].frame >= from) break;
        }
    }
    if (keys[cur].frame > to) return 0;                    // 0x4A2B3C
    int last = 0;
    for (;;) {
        keys[cur].allocated = 1;                           // 0x4A2B50
        last = cur;
        const int next = static_cast<int>(keys[cur].next);
        if (next == 0) break;                              // 0x4A2B60
        if (keys[next].frame > to) break;                  // 0x4A2B79
        cur = next;
    }
    return last;
}

}  // namespace

// ---- VA 0x004A4940 --------------------------------------------------------
void ResetBoneKeyCursor(unsigned char* model) {
    unsigned char* const m = model;
    mdl::BoneKey* const keys = mdl::BoneKeys(m);
    const int capacity = static_cast<int>(mdl::BoneKeyCapacity(*mdl::Mdl(m)));
    int& cursor = mdl::Mdl(m)->searchCursor;
    cursor = static_cast<int>(mdl::Mdl(m)->boneCount);
    while (cursor < capacity &&
           keys[cursor].frame != 0)
        ++cursor;
}

// ---- VA 0x004A49A0 --------------------------------------------------------
int ResetMorphKeyCursor(unsigned char* model) {
    const auto& state = *mdl::Mdl(model);
    const auto* keys = state.morphKeys;
    const auto capacity = mdl::MorphKeyCapacity(keys);
    int& cursor = mdl::Mdl(model)->searchCursor;
    cursor = static_cast<int>(state.morphCount);
    while (std::size_t(cursor) < capacity && keys[cursor].frame != 0) ++cursor;
    // Legacy callers ignore the byte-offset return; avoid signed overflow.
    return cursor <= INT_MAX / 5 ? 5 * cursor : INT_MAX;
}

// The shared cursor may have been left past this pool by a bone operation.
int AllocateMorphKeySlot(unsigned char* model, mdl::MorphKey*& keys,
                         std::size_t& capacity, int& cursor) {
    const int first = static_cast<int>(mdl::Mdl(model)->morphCount);
    if (cursor < first || std::size_t(cursor) >= capacity) cursor = first;
    while (std::size_t(cursor) < capacity && keys[cursor].frame != 0) ++cursor;
    if (std::size_t(cursor) >= capacity) {
        if (!mdl::MorphKeyPoolEnsure(*mdl::Mdl(model), std::uint64_t(capacity) + 1))
            return -1;
        keys = mdl::MorphKeys(model);
        capacity = mdl::MorphKeyCapacity(keys);
    }
    return cursor;
}

// ---- VA 0x0049EEE0 --------------------------------------------------------
// RegisterMorphKeyCurrent (was Sub49EEE0): register the current weight of
// one morph at an absolute frame.  Unlike the VMD/paste registrar below,
// this function takes a morph index directly and reads its live value from
// model+0x26C4[index].weight (+0x30).
void RegisterMorphKeyCurrent(unsigned char* model, int morph, int frameArg) {
    unsigned char* const m = model;
    if (m == nullptr || morph < 0 ||
        (mikumikustudio::mdl::Mdl(m)->physicsMode != 2 && morph == 0) || mikumikustudio::mdl::Morphs(m) == nullptr)
        return;

    mdl::MorphKey* keys = mdl::MorphKeys(m);
    std::size_t capacity = mdl::MorphKeyCapacity(keys);
    if(frameArg<0||frameArg>frame_limits::maximum)return;
    const std::uint32_t frame = static_cast<std::uint32_t>(frameArg);
    const float value = mikumikustudio::mdl::Morphs(m)[morph].value;

    const auto fill = [&](int index) {
        keys[index].frame = frame;
        keys[index].value = value;
        keys[index].allocated = 1;
        if (frame > mdl::Mdl(m)->maxFrame)
            mdl::Mdl(m)->maxFrame = frame;
    };
    int cursor = static_cast<int>(mdl::Mdl(m)->morphCount);
    const auto allocate = [&]() -> int {
        const int index = AllocateMorphKeySlot(m, keys, capacity, cursor);
        if (index < 0) KeyPoolAllocationBox(m);
        return index;
    };

    int current = morph;
    if (keys[current].frame < frame) {
        for (;;) {
            const int next = static_cast<int>(keys[current].next);
            if (next == 0)
                break;
            current = next;
            if (keys[current].frame >= frame)
                break;
        }
        if (keys[current].frame < frame) {
            const int fresh = allocate();
            if (fresh < 0) return;
            keys[current].next = static_cast<std::uint32_t>(fresh);
            keys[fresh].previous = static_cast<std::uint32_t>(current);
            fill(fresh);
            return;
        }
    }

    if (keys[current].frame == frame) {
        keys[current].value = value;
        keys[current].allocated = 1;
        return;
    }

    const int fresh = allocate();
    if (fresh < 0) return;
    const int previous = static_cast<int>(keys[current].previous);
    keys[previous].next = static_cast<std::uint32_t>(fresh);
    keys[fresh].previous = static_cast<std::uint32_t>(previous);
    keys[current].previous = static_cast<std::uint32_t>(fresh);
    keys[fresh].next = static_cast<std::uint32_t>(current);
    fill(fresh);
}

// ---- VA 0x0049D880 (was Sub49D880) -----------------------------------------
bool RegisterBoneKey(unsigned char* model, unsigned char* rec, int frameOffset,
                     unsigned char useSelected) {
    unsigned char* const m = model;
    // Non-const: a free-slot scan below may grow the pool and move it.
    mdl::BoneKey* keys = mdl::BoneKeys(m);
    std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(m));
    std::uint32_t frame;
    if(!frame_limits::Add(frameOffset, Rd32(rec+32), frame))return false;

    // bone lookup by name (or the selected index)
    int boneIdx;
    if (useSelected != 0) {
        boneIdx = mdl::Mdl(m)->selectedBone;
    } else {
        boneIdx = 0;
        const int boneCnt = static_cast<int>(mdl::Mdl(m)->boneCount);
        if (boneCnt <= 0) return true;
        const mdl::BoneRecord* bone = mdl::Bones(m);
        while (std::strcmp(reinterpret_cast<const char*>(rec),
                           bone->name) != 0) {
            if (++boneIdx >= boneCnt) return true;
            ++bone;
        }
    }
    if (boneIdx == -1) return true;

    mdl::BoneRecord& bone = mdl::Bones(m)[boneIdx];
    const mdl::BoneType btype = bone.type;
    const bool writePos = btype == mdl::BoneType::RotateMove ||
                          btype == mdl::BoneType::Move ||
                          btype == mdl::BoneType::Ik;

    // twist correction: force the rotation axis onto the bone's own axis
    if (btype == mdl::BoneType::FixedAxis ||
        (btype == mdl::BoneType::UnderIk &&
         (bone.flags & mdl::kBoneFlagFixedAxis) == mdl::kBoneFlagFixedAxis)) {
        float boneAxis[3];
        if (mikumikustudio::mdl::Mdl(m)->physicsMode == 2) {  // PMX
            std::memcpy(boneAxis, bone.axis, sizeof boneAxis);
        } else {
            const mdl::BoneRecord& tail = mdl::Bones(m)[bone.tailBone];
            boneAxis[0] = tail.position[0] - bone.position[0];
            boneAxis[1] = tail.position[1] - bone.position[1];
            boneAxis[2] = tail.position[2] - bone.position[2];
            auto* d3 = &d3dx::Get();
            if (d3->module != nullptr) d3->vec3Normalize(boneAxis, boneAxis);
        }
        float* const q = reinterpret_cast<float*>(rec + 36);
        float axisOut[3], angle;
        {
            auto* d3 = &d3dx::Get();
            if (d3->quatToAxisAngle == nullptr) return false;
            d3->quatToAxisAngle(q, axisOut, &angle);
        }
        if (!_finite(q[3])) q[3] = 1.0f;  // w
        float sinHalf;
        const float ww = q[3] * q[3];
        if (ww <= 1.0f) {
            sinHalf = sqrtf(1.0f - ww);
        } else {
            q[3] = 1.0f;
            sinHalf = 0.0f;
        }
        const float dot = boneAxis[0] * axisOut[0] +
                          boneAxis[1] * axisOut[1] +
                          boneAxis[2] * axisOut[2];
        const float nA = sqrtf(boneAxis[0] * boneAxis[0] +
                               boneAxis[1] * boneAxis[1] +
                               boneAxis[2] * boneAxis[2]);
        const float nB = sqrtf(axisOut[0] * axisOut[0] +
                               axisOut[1] * axisOut[1] +
                               axisOut[2] * axisOut[2]);
        const float cosT = dot / (nB * nA);
        if (cosT < 0.0f) sinHalf = -sinHalf;
        // Original x64 0x1400C96EF..0x1400C9719 multiplies by the bone
        // axis (xmm11/xmm10/xmm12), not the source quaternion's xyz.
        // D3DXQuaternionToAxisAngle copies xyz without normalizing it;
        // using axisOut here shrinks angles and reverses negative twists.
        q[0] = sinHalf * boneAxis[0];
        q[1] = sinHalf * boneAxis[1];
        q[2] = sinHalf * boneAxis[2];
        auto* d3 = &d3dx::Get();
        if (d3->quatNormalize != nullptr) d3->quatNormalize(q, q);
    }

    // walk the per-bone chain from the bone index itself
    int cur = boneIdx;
    if (keys[cur].frame < frame) {
        for (;;) {
            const int next = static_cast<int>(keys[cur].next);
            if (next == 0) break;
            cur = next;
            if (keys[cur].frame >= frame) break;
        }
        if (keys[cur].frame < frame) {
            // Append after the last record (0x49DDA1 path).
            // Deliberate hardening: the original reads keys[cursor].frame
            // before checking the cursor against the pool end (verified at
            // 0x1400C9958), so a full pool reads one record past the array.
            // This port checks the cursor first and grows the pool instead of
            // reporting the overflow while it still can.
            // Defect MB-20.
            const int free_ = AllocateBoneKeySlot(m, keys, boneKeyCapacity);
            if (free_ < 0) {
                KeyPoolAllocationBox(m);
                return false;
            }
            AppendBoneKeyToUndo(m, cur);
            AppendBoneKeyToUndo(m, free_);
            keys[cur].next = static_cast<std::uint32_t>(free_);
            keys[free_].previous = static_cast<std::uint32_t>(cur);
            keys[free_].frame = frame;
            bone_scale::ApplyRegistration(model, free_, true);
            FillBoneRecord(keys[free_], rec, writePos);
            bone_curve::ReconcileRegisteredFrame(model,boneIdx,int(frame));
            if (frame > mdl::Mdl(m)->maxFrame)
                mdl::Mdl(m)->maxFrame = frame;
            return true;
        }
    }

    // at/before the found record (0x49DBE4 path)
    if (frame == keys[cur].frame) {
        // exact frame: overwrite in place
        AppendBoneKeyToUndo(m, cur);
        bone_scale::ApplyRegistration(model, cur, false);
        FillBoneRecord(keys[cur], rec, writePos);
    } else {
        // insert before cur
        // Deliberate hardening: the original reads keys[cursor].frame
        // before checking the cursor against the pool end (verified at 0x1400C9B3D),
        // so a full pool reads one record past the array. The port checks the
        // cursor first and grows the pool instead of reporting the overflow
        // while it still can.
        // Defect MB-20.
        const int free_ = AllocateBoneKeySlot(m, keys, boneKeyCapacity);
        if (free_ < 0) {
            KeyPoolAllocationBox(m);
            return false;
        }
        const int prev = static_cast<int>(keys[cur].previous);
        AppendBoneKeyToUndo(m, prev);
        AppendBoneKeyToUndo(m, cur);
        AppendBoneKeyToUndo(m, free_);
        keys[prev].next = static_cast<std::uint32_t>(free_);
        keys[free_].previous = static_cast<std::uint32_t>(prev);
        keys[cur].previous = static_cast<std::uint32_t>(free_);
        keys[free_].next = static_cast<std::uint32_t>(cur);
        keys[free_].frame = frame;
        bone_scale::ApplyRegistration(model, free_, true);
        FillBoneRecord(keys[free_], rec, writePos);
    }
    // max-frame bump uses the RAW frameOffset here (original quirk)
    bone_curve::ReconcileRegisteredFrame(model,boneIdx,int(frame));
    if (static_cast<std::uint32_t>(frameOffset) > mdl::Mdl(m)->maxFrame)
        mdl::Mdl(m)->maxFrame = static_cast<std::uint32_t>(frameOffset);
    return true;
}

// ---- VA 0x0049E310 (was Sub49E310) -----------------------------------------
// Register a left/right-reflected bone key. This is frame-edit command 422
// ("reverse paste"), not a delete routine.
bool RegisterMirroredBoneKey(unsigned char* model, unsigned char* rec,
                             int frameOffset) {
    unsigned char* const m = model;
    // Non-const: a free-slot scan below may grow the pool and move it.
    mdl::BoneKey* keys = mdl::BoneKeys(m);
    std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(m));
    std::uint32_t frame;
    if(!frame_limits::Add(frameOffset, Rd32(rec+32), frame))return false;
    const int boneIdx = FindMirroredBone(m, rec);
    if (boneIdx < 0) return true;

    mdl::BoneRecord& bone = mdl::Bones(m)[boneIdx];
    const mdl::BoneType btype = bone.type;
    if (btype == mdl::BoneType::FixedAxis ||
        (btype == mdl::BoneType::UnderIk &&
         (bone.flags & mdl::kBoneFlagFixedAxis) == mdl::kBoneFlagFixedAxis)) {
        float boneAxis[3];
        if (mikumikustudio::mdl::Mdl(m)->physicsMode == 2) {
            std::memcpy(boneAxis, bone.axis, sizeof boneAxis);
        } else {
            const mdl::BoneRecord& tail = mdl::Bones(m)[bone.tailBone];
            boneAxis[0] = tail.position[0] - bone.position[0];
            boneAxis[1] = tail.position[1] - bone.position[1];
            boneAxis[2] = tail.position[2] - bone.position[2];
            auto* d3 = &d3dx::Get();
            if (d3->vec3Normalize != nullptr)
                d3->vec3Normalize(boneAxis, boneAxis);
        }

        float* const q = reinterpret_cast<float*>(rec + 36);
        float sourceAxis[3], angle;
        auto* d3 = &d3dx::Get();
        if (d3->quatToAxisAngle == nullptr) return false;
        d3->quatToAxisAngle(q, sourceAxis, &angle);
        if (!_finite(q[3])) q[3] = 1.0f;
        float sinHalf;
        const float ww = q[3] * q[3];
        if (ww <= 1.0f) {
            sinHalf = sqrtf(1.0f - ww);
        } else {
            q[3] = 1.0f;
            sinHalf = 0.0f;
        }
        sourceAxis[0] = -sourceAxis[0];
        sinHalf = -sinHalf;
        const float dot = boneAxis[0] * sourceAxis[0] +
                          boneAxis[1] * sourceAxis[1] +
                          boneAxis[2] * sourceAxis[2];
        const float nA = sqrtf(boneAxis[0] * boneAxis[0] +
                               boneAxis[1] * boneAxis[1] +
                               boneAxis[2] * boneAxis[2]);
        const float nB = sqrtf(sourceAxis[0] * sourceAxis[0] +
                               sourceAxis[1] * sourceAxis[1] +
                               sourceAxis[2] * sourceAxis[2]);
        if (dot / (nA * nB) < 0.0f) sinHalf = -sinHalf;
        q[0] = sinHalf * boneAxis[0];
        q[1] = -sinHalf * boneAxis[1];
        q[2] = -sinHalf * boneAxis[2];
        if (d3->quatNormalize != nullptr) d3->quatNormalize(q, q);
    }

    int cur = boneIdx;
    if (keys[cur].frame < frame) {
        for (;;) {
            const int next = static_cast<int>(keys[cur].next);
            if (next == 0) break;
            cur = next;
            if (keys[cur].frame >= frame) break;
        }
        if (keys[cur].frame < frame) {
            // Deliberate hardening: the original reads keys[cursor].frame
            // before checking the cursor against the pool end (verified at
            // 0x1400CA46F), so a full pool reads one record past the array.
            // The port checks the cursor first and grows the pool instead of
            // reporting the overflow while it still can.
            // Defect MB-20.
            const int free_ = AllocateBoneKeySlot(m, keys, boneKeyCapacity);
            if (free_ < 0) {
                KeyPoolAllocationBox(m);
                return false;
            }
            AppendBoneKeyToUndo(m, cur);
            AppendBoneKeyToUndo(m, free_);
            keys[cur].next = static_cast<std::uint32_t>(free_);
            keys[free_].previous = static_cast<std::uint32_t>(cur);
            keys[free_].frame = frame;
            bone_scale::ApplyRegistration(model, free_, true);
            FillMirroredBoneRecord(keys[free_], rec);
            bone_curve::ReconcileRegisteredFrame(model,boneIdx,int(frame));
            if (frame > mdl::Mdl(m)->maxFrame)
                mdl::Mdl(m)->maxFrame = frame;
            return true;
        }
    }

    if (frame == keys[cur].frame) {
        AppendBoneKeyToUndo(m, cur);
        bone_scale::ApplyRegistration(model, cur, false);
        FillMirroredBoneRecord(keys[cur], rec);
    } else {
        // Deliberate hardening: the original code reads keys[cursor].frame before
        // checking the cursor against the pool end (verified at 0x1400CA6B9),
        // so a full pool reads one record beyond the array. The port tests the
        // cursor first and grows the pool instead of reporting the overflow
        // while it still can.
        // Defect MB-20.
        const int free_ = AllocateBoneKeySlot(m, keys, boneKeyCapacity);
        if (free_ < 0) {
            KeyPoolAllocationBox(m);
            return false;
        }
        const int prev = static_cast<int>(keys[cur].previous);
        AppendBoneKeyToUndo(m, prev);
        AppendBoneKeyToUndo(m, cur);
        AppendBoneKeyToUndo(m, free_);
        keys[prev].next = static_cast<std::uint32_t>(free_);
        keys[free_].previous = static_cast<std::uint32_t>(prev);
        keys[cur].previous = static_cast<std::uint32_t>(free_);
        keys[free_].next = static_cast<std::uint32_t>(cur);
        keys[free_].frame = frame;
        bone_scale::ApplyRegistration(model, free_, true);
        FillMirroredBoneRecord(keys[free_], rec);
    }
    bone_curve::ReconcileRegisteredFrame(model,boneIdx,int(frame));
    if (static_cast<std::uint32_t>(frameOffset) > mdl::Mdl(m)->maxFrame)
        mdl::Mdl(m)->maxFrame = static_cast<std::uint32_t>(frameOffset);
    return true;
}

// ---- VA 0x0049F190 --------------------------------------------------------
// was Sub49F190 - name-matched morph registrar over a parsed 0x28 record.
bool RegisterMorphKeyFromRecord(unsigned char* model, const unsigned char* rec,
                                int frameOffset) {
    unsigned char* const m = model;
    std::uint32_t frame;
    if(!frame_limits::Add(frameOffset, Rd32(rec+32), frame))return false;

    int morphIdx = 0;
    const int morphCnt = static_cast<int>(mdl::Mdl(m)->morphCount);
    if (morphCnt <= 0) return true;
    const mikumikustudio::mdl::MorphRecord* mrec = mikumikustudio::mdl::Morphs(m);
    if (mrec == nullptr) return true;
    while (std::strncmp(reinterpret_cast<const char*>(rec),
                        mrec->name, sizeof(mrec->name)) != 0) {
        if (++morphIdx >= morphCnt) return true;
        ++mrec; // typed records already account for the x86/x64 stride
    }
    return RegisterMorphKeyAtIndex(model, morphIdx, frame, RdF32(rec + 36));
}

// Explicit destination identity keeps force paste independent of names, even
// when two expressions have the same display or encoded name.
bool RegisterMorphKeyAtIndex(unsigned char* model, int morphIdx,
                             std::uint32_t frame, float value) {
    unsigned char* const m = model;
    if(frame>std::uint32_t(frame_limits::maximum))return false;
    if (!m || morphIdx < 0 || static_cast<unsigned>(morphIdx) >= mdl::Mdl(m)->morphCount)
        return false;
    mdl::MorphKey* keys = mdl::MorphKeys(m);
    std::size_t capacity = mdl::MorphKeyCapacity(keys);
    if (keys == nullptr) return false;

    int cur = morphIdx;
    if (keys[cur].frame < frame) {
        for (;;) {
            const int next = static_cast<int>(keys[cur].next);
            if (next == 0) break;
            cur = next;
            if (keys[cur].frame >= frame) break;
        }
        if (keys[cur].frame < frame) {
            // append
            const int free_ = AllocateMorphKeySlot(m, keys, capacity, mdl::Mdl(m)->searchCursor);
            if (free_ < 0) {
                KeyPoolAllocationBox(m);
                return false;
            }
            keys[cur].next = static_cast<std::uint32_t>(free_);
            keys[free_].previous = static_cast<std::uint32_t>(cur);
            keys[free_].frame = frame;
            keys[free_].value = value;
            keys[free_].allocated = 1;
            if (frame > mdl::Mdl(m)->maxFrame)
                mdl::Mdl(m)->maxFrame = frame;
            return true;
        }
    }

    if (frame == keys[cur].frame) {
        keys[cur].value = value;
        keys[cur].allocated = 1;
        return true;
    }
    // insert before cur
    const int free_ = AllocateMorphKeySlot(m, keys, capacity, mdl::Mdl(m)->searchCursor);
            if (free_ < 0) {
        KeyPoolAllocationBox(m);
        return false;
    }
    const int prev = static_cast<int>(keys[cur].previous);
    keys[prev].next = static_cast<std::uint32_t>(free_);
    keys[free_].previous = static_cast<std::uint32_t>(prev);
    keys[cur].previous = static_cast<std::uint32_t>(free_);
    keys[free_].next = static_cast<std::uint32_t>(cur);
    keys[free_].frame = frame;
    keys[free_].value = value;
    keys[free_].allocated = 1;
    if (frame > mdl::Mdl(m)->maxFrame)
        mdl::Mdl(m)->maxFrame = frame;
    return true;
}

// ---- VA 0x0049F8C0 --------------------------------------------------------
// was Sub49F8C0 - display/IK registrar over parsed 21-/28-byte entries.
bool RegisterDisplayKeyFromRecord(unsigned char* model, int frameArg,
                                  unsigned char view, int cnt,
                                  const unsigned char* entries21, int cnt2,
                                  const unsigned char* entries28,
                                  int frameOffset) {
    unsigned char* const m = model;
    mdl::DisplayKey*& keys = mdl::DisplayKeys(m);
    std::uint32_t frame;
    if(frameArg<0 || !frame_limits::Add(frameOffset, std::uint32_t(frameArg), frame))return false;
    // 注意：x64（sub_7FF7CB4EB3C0 完整反编译）注册显示键时 *不* 写
    // displayKeyframesPresent。该字段是左面板 表示/IK 行的轨道选中开关，
    // 只在行点击时翻转（0x7FF7CB45A2DD..0x7FF7CB45A2EA，port 见
    // ui_editor_click.cpp），控制关键帧跳转命令的搜索范围。

    // fill the per-IK byte array and the selector pair array of the
    // record at index (shared by all three insert paths)
    auto FillEntries = [&](int index) {
        const int ikCnt = static_cast<int>(mdl::Mdl(m)->ikChainCount);
        auto* const ikBase = mdl::IkStates(keys[index]);
        for (int i = 0; i < ikCnt; ++i) {
            if (cnt > 0) {
                if(extended_names::importedIkTargets){
                    const auto& targets=*extended_names::importedIkTargets;
                    for(int k=0;k<cnt&&std::size_t(k)<targets.size();++k)
                        if(targets[k]==mdl::IkChains(m)[i].boneIndex){ikBase[i]=entries21[21*std::size_t(k)+20];break;}
                    continue;
                }
                const unsigned char* e = entries21;
                const unsigned char* const chainBone =
                    reinterpret_cast<const unsigned char*>(
                        mdl::Bones(m) + mdl::IkChains(m)[i].boneIndex);
                int k = 0;
                // Both names are fixed 20-byte fields. A full-width name has
                // no terminator: its following byte is the serialized IK
                // switch (or the bone's English name), not part of its name.
                while (std::strncmp(reinterpret_cast<const char*>(e),
                                    reinterpret_cast<const char*>(chainBone),
                                    sizeof(mdl::BoneRecord::name)) != 0) {
                    ++k;
                    e += 21;
                    if (k >= cnt)
                        break;
                }
                // The search cursor advances for the name comparison, but the original
                // value load at 0x49FCA4/0x49FB7A/0x49FF58 re-indexes from
                // the unmodified a5 base (imul index, 0x15), not from the
                // already-advanced cursor.
                if (k < cnt)
                    ikBase[i] = entries21[21 * k + 20];
            }
        }
        const int selCnt = static_cast<int>(mdl::Mdl(m)->boneOrderCount);
        mdl::BoneReference* const pairBase =
            mdl::SelectorStates(keys[index]);
        const mdl::BoneOrderEntry* const relationships = mdl::BoneOrder(m);
        for (int i = 0; i < selCnt; ++i) {
            if (cnt2 > 0) {
                // Slot zero is the model-wide parent, whose bone index is -1.
                // Clipboard and range-edit records put its payload first;
                // it has no bone name to look up in the model's bone table.
                if (i == 0) {
                    pairBase[0].modelIndex = RdI32(entries28 + 20);
                    pairBase[0].boneIndex = RdI32(entries28 + 24);
                    continue;
                }
                const unsigned char* e = entries28;
                const unsigned char* const selBone =
                    reinterpret_cast<const unsigned char*>(mdl::Bones(m) +
                                                            relationships[i].boneIndex);
                int k = 0;
                while (std::strcmp(reinterpret_cast<const char*>(e),
                                   reinterpret_cast<const char*>(
                                       selBone)) != 0) {
                    ++k;
                    e += 28;
                    if (k >= cnt2)
                        break;
                }
                if (k < cnt2) {
                    // Likewise 0x49FD4D..0x49FD7F and its two sibling paths
                    // derive the payload address from the original a7 base.
                    pairBase[i].modelIndex =
                        RdI32(entries28 + 28 * k + 20);
                    pairBase[i].boneIndex =
                        RdI32(entries28 + 28 * k + 24);
                }
            }
        }
    };

    int cur = 0;
    if (keys[0].frame < frame) {
        for (;;) {
            const int next = static_cast<int>(keys[cur].next);
            if (next == 0) break;
            cur = next;
            if (keys[cur].frame >= frame) break;
        }
        if (keys[cur].frame < frame) {
            // append
            std::uint32_t free_ = 0;
            if (!mdl::DisplayKeyFreeSlot(*mdl::Mdl(m), free_)) {
                OverflowBox(m, static_cast<int>(mdl::KeyPoolCapacity(keys)));
                return false;
            }
            keys[cur].next = static_cast<std::uint32_t>(free_);
            keys[free_].previous = static_cast<std::uint32_t>(cur);
            keys[free_].frame = frame;
            keys[free_].visible = view;
            FillEntries(free_);
            keys[free_].allocated = 1;
            if (frame > mdl::Mdl(m)->maxFrame)
                mdl::Mdl(m)->maxFrame = frame;
            return true;
        }
    }

    if (frame == keys[cur].frame) {
        keys[cur].visible = view;
        FillEntries(cur);
        keys[cur].allocated = 1;
        return true;
    }
    // insert before cur
    std::uint32_t free_ = 0;
    if (!mdl::DisplayKeyFreeSlot(*mdl::Mdl(m), free_)) {
        OverflowBox(m, static_cast<int>(mdl::KeyPoolCapacity(keys)));
        return false;
    }
    const int prev = static_cast<int>(keys[cur].previous);
    keys[prev].next = static_cast<std::uint32_t>(free_);
    keys[free_].previous = static_cast<std::uint32_t>(prev);
    keys[cur].previous = static_cast<std::uint32_t>(free_);
    keys[free_].next = static_cast<std::uint32_t>(cur);
    keys[free_].frame = frame;
    keys[free_].visible = view;
    FillEntries(free_);
    keys[free_].allocated = 1;
    if (frame > mdl::Mdl(m)->maxFrame)
        mdl::Mdl(m)->maxFrame = frame;
    return true;
}

// ---- VA 0x0049F480 --------------------------------------------------------
// RegisterDisplayKeyCurrent (was Sub49F480): register the model-wide
// display/IK/relationship state at one frame.  The record owns two arrays
// allocated by ModelInitDefaults: one byte per IK chain at +16 and one
// {dword,dword} pair per relationship at +24.
void RegisterDisplayKeyCurrent(unsigned char* model, int frameArg) {
    unsigned char* const m = model;
    mdl::DisplayKey*& keys = mdl::DisplayKeys(m);
    if(frameArg<0||frameArg>frame_limits::maximum)return;
    const std::uint32_t frame = static_cast<std::uint32_t>(frameArg);
    // 同上：x64（sub_7FF7CB4EAF80）此处也不写 displayKeyframesPresent——
    // 它是左面板 表示/IK 行的选中开关（点击翻转，0x7FF7CB45A2DD..EA），
    // 不是“模型拥有显示键”的存在标志。

    const auto fill = [&](int index) {
        mdl::DisplayKey& rec = keys[index];
        rec.visible = mikumikustudio::mdl::Mdl(m)->loadComplete;
        auto* const ik = mdl::IkStates(rec);
        mdl::IkChain* const chains = mdl::IkChains(m);
        for (std::uint32_t i = 0; i < mdl::Mdl(m)->ikChainCount; ++i)
            ik[i] = chains[i].enabled;
        mdl::BoneReference* const pairs = mdl::SelectorStates(rec);
        mdl::BoneOrderEntry* const relationships = mdl::BoneOrder(m);
        for (std::uint32_t i = 0; i < mdl::Mdl(m)->boneOrderCount; ++i) {
            pairs[i].modelIndex = relationships[i].linkedModel;
            pairs[i].boneIndex = relationships[i].linkedBone;
        }
        rec.allocated = 1;
    };

    int current = 0;
    if (keys[0].frame < frame) {
        for (;;) {
            const int next = static_cast<int>(keys[current].next);
            if (next == 0)
                break;
            current = next;
            if (keys[current].frame >= frame)
                break;
        }
    }
    if (keys[current].frame == frame) {
        fill(current);
        return;
    }

    std::uint32_t freeIndex = 0;
    if (!mdl::DisplayKeyFreeSlot(*mdl::Mdl(m), freeIndex)) {
        OverflowBox(m, static_cast<int>(mdl::KeyPoolCapacity(keys)));
        return;
    }

    if (keys[current].frame < frame) {
        // Append after the terminal record.
        keys[current].next = static_cast<std::uint32_t>(freeIndex);
        keys[freeIndex].previous = static_cast<std::uint32_t>(current);
    } else {
        // Insert immediately before current.
        const int previous = static_cast<int>(keys[current].previous);
        keys[previous].next = static_cast<std::uint32_t>(freeIndex);
        keys[freeIndex].previous = static_cast<std::uint32_t>(previous);
        keys[current].previous = static_cast<std::uint32_t>(freeIndex);
        keys[freeIndex].next = static_cast<std::uint32_t>(current);
    }
    keys[freeIndex].frame = frame;
    fill(freeIndex);
    if (frame > mdl::Mdl(m)->maxFrame)
        mdl::Mdl(m)->maxFrame = frame;
}

// ---- VA 0x004A27F0 (was Sub4A27F0) ----------------------------------------
// Name-based key-track frame marker (x64 sub_7FF7CB4EFED0).  See the file
// header for the resolution chain.  Walks are unsigned frame compares over
// the per-track next-linked chains, exactly like the registrar family
// above; the first branch that matches owns the walk and returns.
int MarkKeyTrackRangeByName(unsigned char* model, std::uint32_t from,
                            std::uint32_t to, const char* name) {
    mdl::ModelRecord& record = *mdl::Mdl(model);

    // 1. facial display-frame groups (0x4A2806..0x4A2855): SJIS name only
    //    (0x4A2820 inline strcmp), targetIndex roots the morph track.
    const int facialCount = mikumikustudio::mdl::FacialFrameCount(record);
    if (facialCount > 0) {
        mdl::FrameGroup* const groups = mdl::DisplayFrames(model);
        for (int g = 0; g < facialCount; ++g) {
            if (std::strcmp(name, groups[g].name) == 0) {
                // Deliberate hardening: the original uses the display row's
                // targetIndex directly as the root record of the morph track
                // without checking it against the pool size (verified at
                // 0x1400D000B and 0x1400D0050); the field is 16-bit and
                // can reach 65535 while the morph pool holds 20000 records.
                // This port rejects such a root and returns the same
                // "nothing marked" result.
                // Defect MB-19.
                const int root = static_cast<int>(mikumikustudio::mdl::FrameTargetIndex(groups[g]));
                if (root >= static_cast<int>(mdl::MorphKeyCapacity(mdl::MorphKeys(model))))
                    return 0;
                return MarkTrackRange(mdl::MorphKeys(model), root, from, to);
            }
        }
    }

    // 2. 表示･IK･外親 / "disp/IK/OP" (0x4A285F..0x4A2892): display track.
    if (std::memcmp(name, kJpDispIkOp, 13) == 0 ||
        std::strcmp(name, "disp/IK/OP") == 0) {
        return MarkTrackRange(mdl::DisplayKeys(model), 0, from, to);
    }

    // 3. the root bone's own name, SJIS or English (0x4A29B2..0x4A2A25) -
    //    combo 434's center-bone entry -> bone-key record 0.
    const mdl::BoneRecord* const rootBone = mdl::Bones(model);
    if (std::strcmp(name, rootBone->name) == 0 ||
        std::strcmp(name, rootBone->nameEn) == 0) {
        return MarkTrackRange(mdl::BoneKeys(model), 0, from, to);
    }

    // 4. ﾎﾞｰﾝ01 / "bone01" (0x4A2AB8..0x4A2AEB): the bundled ダミーボーン
    //    model's combo entries -> bone-key record 0.
    if (std::memcmp(name, kJpBone01, 7) == 0 ||
        std::strcmp(name, "bone01") == 0) {
        return MarkTrackRange(mdl::BoneKeys(model), 0, from, to);
    }

    // 5. bone display-frame groups (0x4A2B88..0x4A2C25): SJIS or English
    //    name, targetIndex roots the bone track.
    const std::int32_t groupCount =
        static_cast<std::int32_t>(record.rigidBodyCount);
    if (groupCount > 0) {
        mdl::FrameGroup* const groups = mdl::RigidGroups(model);
        for (std::int32_t g = 0; g < groupCount; ++g) {
            if (std::strcmp(name, groups[g].name) == 0 ||
                std::strcmp(name, groups[g].nameEn) == 0) {
                // Deliberate hardening: the original code uses the display row's
                // targetIndex directly as the root record of the bone track,
                // without checking it against the pool size (verified at
                // 0x1400D02A5 and 0x1400D02E0).  This port bounds the root
                // index against the bone-key pool rather than traversing from an
                // unchecked index.
                // Defect MB-19.
                const int root = static_cast<int>(mikumikustudio::mdl::FrameTargetIndex(groups[g]));
                if (root < 0 ||
                    static_cast<std::size_t>(root) >=
                        mdl::BoneKeyCapacity(record))
                    return 0;
                return MarkTrackRange(mdl::BoneKeys(model), root, from, to);
            }
        }
    }
    return 0;
}

}  // namespace mikumikustudio
