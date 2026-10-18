#include "mikudancestudio/key_pool.hpp"
// ===========================================================================
// Gap ports: wave-context reset, physics-pose key registration and the
// standard-skeleton base-quaternion initializer
//   VA 0x004C2430 - ClearWaveContextFields (was Sub4C2430)
//                              wave/timeline 0x25C-context field clear
//   VA 0x004A4A50 - RegisterPhysicsPoseChain (was Sub4A4A50)
//                              physics-pose key registrar (per-bone chain,
//                              model+14584 consecutive frames)
//   VA 0x004A5690 - RegisterPhysicsPose (was Sub4A5690)
//                              "register physics pose" driver (undo snapshot
//                              + standard-bone probes -> RegisterPhysicsPoseChain)
//   VA 0x004A60F0 - BuildLookAtQuaternion (was Sub4A60F0)
//                              look-at quaternion builder (__stdcall)
//   VA 0x004A6520 - InitStandardSkeletonQuats (was Sub4A6520)
//                              standard-skeleton base quaternion setup
// ===========================================================================
// Scope note: the other VAs handed out with this batch were adjudicated as
// already covered and are NOT re-ported here:
//   0x4A48D0 / 0x4A48E0  inlined in src/exports/effect_api.cpp
//     (ExpGetPmdMatNum ~line 267 reads model+0x1C; ExpGetPmdMaterial
//     ~line 275 copies 17 floats from the 0x8F4-stride records with the
//     same inclusive `<=` bound).
//   0x4C2470  replaced by lazy device creation in src/media/wave_audio.cpp.
//     UI startup binds only the timeline HDC and window. A loaded audio
//     file triggers DirectSoundCreate + SetCooperativeLevel.
//   0x4C26F0 / 0x4C2C90 ported in src/media/wave_audio.cpp (WaveFindDataChunk
//     line 138, WaveStreamFeed line 192).
//
// Model-object offsets used by the 0x4A4A50/0x4A5690/0x4A6520 family
// (literal hex; effect_api.cpp and key_registrars.cpp agree on the
// shared ones):
//   model+0x0000    HWND owner (message box parent)
//   model+0x0040    (+64)  upper-body base quaternion          (0x4A6520)
//   model+0x0050    (+80)  neck quaternion                     (0x4A6520)
//   model+0x0060..+0x00B4 (+96..180) arm-chain quats           (0x4A6520)
//   model+0x00C0    (+192) lower-body base quaternion          (0x4A6520)
//   model+0x00D0..+0x012C (+208..300) leg-chain quats          (0x4A6520)
//   model+0x0130/+0x0140 (+304/+320) shoulder mid quats        (0x4A6520)
//   model+0x21AC    (+8620) physics-pose source array ptr
//                   (308-byte/77-float stride slots, 3456 B per undo slot)
//   model+0x26EC    (+9952) bone key array base (60-byte records:
//                   frame +0, prev +4, next +8, ctrl bytes +12..27,
//                   pos +28..39, quat +40..55, mark +56, ik-mode +57)
//   model+0x26F0    (+9956) morph key array (20-byte records)
//   model+0x26F4    (+9960) master/display key array (28-byte records)
//   model+0x2700    (+9964) undo slot records, 28 bytes each:
//                   +0 type (2 = pose registration), +4 dirty dword,
//                   +12 frame, +16 bone-pose ptr (36 B/bone),
//                   +20 pose-source ptr (3456 B/slot)
//   model+0x2D84    (+11652) bone count
//   model+0x2D8C    (+11672) per-bone byte array (undo pose copy)
//   model+0x31B4    (+12724) undo ring index (wraps at 30)
//   model+0x31B8    (+12728) undo ring mirror
//   model+0x31C0    (+12720) max registered frame
//   model+0x31C4    (+12740) English-UI flag byte
//   model+0x31BC    (+12732)/(+12733) pose-registration flags
//   model+0x38FD    (+14589) openniVersion - model-spec gate (>=14 / >=15)
//   model+0x38F8    (+14584) physics-pose slot count
//   model+0x3908    (+14596) bone-key marker array, one byte per record
//                   (memset 0 over kBoneKeyCapacity bytes; x64 twin at
//                   +0x3CAC per the model+0x2790 pool doubling)
//   model+0x38E4    (+14568) computed leg length (cache, -1 = unset)
//   model+0x37CC..0x38F0 (+14284..14568) standard-bone probe positions
//                   (float triples, -999.0 = bone not found)
//
// All receivers are __thiscall(model) in the original; ported as free
// functions taking the model base pointer.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/bone_scale.hpp"
#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/wave_audio_context.hpp"
#include "mikudancestudio/panel_controls.hpp"

#include "../model/keyframe_common.hpp"

namespace mikudancestudio {
namespace {

using kfa::Rd32;
using kfa::RdI32;
using kfa::RdF32;
using kfa::Wr32;
using kfa::WrF32;
using kfa::RdPtr;

// D3DXQuaternionRotationAxis / D3DXQuaternionInverse are not bound in the
// shared d3dx_dyn.hpp Api (which must not be edited this session); resolve
// them locally from the same module handle.
using FnQuatInverseLocal = float*(WINAPI*)(float out[4], const float q[4]);
using FnQuatRotationAxisLocal =
    float*(WINAPI*)(float out[4], const float axis[3], float angle);

FnQuatInverseLocal LocalQuatInverse() {
    static FnQuatInverseLocal fn = reinterpret_cast<FnQuatInverseLocal>(
        GetProcAddress(d3dx::Get().module, "D3DXQuaternionInverse"));
    return fn;
}
FnQuatRotationAxisLocal LocalQuatRotationAxis() {
    static FnQuatRotationAxisLocal fn =
        reinterpret_cast<FnQuatRotationAxisLocal>(GetProcAddress(
            d3dx::Get().module, "D3DXQuaternionRotationAxis"));
    return fn;
}

float* IdentityQuaternion(float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    return out;
}
float ClampUnit(float value) {
    return value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
}
bool NormalizeDirection(float direction[3]) {
    const double length = std::sqrt(double(direction[0]) * direction[0] +
        double(direction[1]) * direction[1] + double(direction[2]) * direction[2]);
    if (!std::isfinite(length) || length == 0.0) {
        direction[0] = direction[1] = direction[2] = 0.0f;
        return false;
    }
    for (int i = 0; i < 3; ++i) direction[i] = static_cast<float>(direction[i] / length);
    return true;
}
void NormalizeCoordinates(float& x, float& y, float& z) {
    // Independent local floats are not a float[3], regardless of stack layout.
    float direction[3] = {x, y, z};
    NormalizeDirection(direction);
    x = direction[0]; y = direction[1]; z = direction[2];
}
void RotationAxisFromDot(float out[4], float axis[3], float dot,
                         float fromX, float fromY, float fromZ) {
    if (!std::isfinite(dot)) { IdentityQuaternion(out); return; }
    if (!NormalizeDirection(axis)) {
        if (dot >= 0.0f) { IdentityQuaternion(out); return; }
        // Opposite vectors have no unique cross-product axis. Choose a
        // deterministic perpendicular axis for their half turn.
        if (std::fabs(fromX) <= std::fabs(fromY) && std::fabs(fromX) <= std::fabs(fromZ)) {
            axis[0] = 0.0f; axis[1] = fromZ; axis[2] = -fromY;
        } else if (std::fabs(fromY) <= std::fabs(fromZ)) {
            axis[0] = -fromZ; axis[1] = 0.0f; axis[2] = fromX;
        } else {
            axis[0] = fromY; axis[1] = -fromX; axis[2] = 0.0f;
        }
        if (!NormalizeDirection(axis)) { IdentityQuaternion(out); return; }
    }
    LocalQuatRotationAxis()(out, axis, std::acos(ClampUnit(dot)));
}

// .rdata float constants (bit-exact; Hex-Rays decimal renderings do not
// round-trip, e.g. flt_52B738 = 0x40490FD8 prints as "3.141592" but the
// float nearest 3.141592 is 0x40490FD7):
constexpr float kPi52B738 = 3.141592f;      // 0x40490FD8 (0x52B738)
// C++17 has no std::bit_cast; MSVC's __builtin_bit_cast is accepted in
// constant expressions, so the static_assert pins the literal above to
// the original .rdata bit pattern.
static_assert(__builtin_bit_cast(std::uint32_t, kPi52B738) == 0x40490FD8u,
              "flt_52B738 bit-exact");
constexpr float k35deg5311A4 = 0.61086512f; // 0x3F1C61A8 (0x5311A4)
constexpr float k30deg53119C = 0.52359867f; // 0x3F060A90 (0x53119C)
constexpr float kNeg0d7 = -0.69999999f;     // 0xBF333333 (0x531190)
constexpr float kNeg0d3 = -0.30000001f;     // 0xBE99999A (0x531198)
constexpr float k75f = 75.0f;               // 0x42960000 (0x531194)
constexpr float kSentinel = -999.0;         // dbl 0x5311B0 (x87 compares
                                             // float mem against the double)

// JP overflow strings 0x52B918 / 0x52B908 (SJIS byte-exact, shared with
// key_registrars.cpp's OverflowBox).
const char kJpOverflow[] =
    "\x93\x6f\x98\x5e\x83\x7c\x83\x43\x83\x93\x83\x67\x90\x94\x82\xaa%d"
    "\x8c\xc2\x82\xf0\x89\x7a\x82\xa6\x82\xdc\x82\xb5\x82\xbd\n"
    "\x82\xb1\x82\xea\x88\xc8\x8f\xe3\x82\xcc\x93\x6f\x98\x5e\x82\xcd\x8d"
    "\x73\x82\xa6\x82\xdc\x82\xb9\x82\xf1\n"
    "\x81\x75\xcc\xda\xb0\xd1\x95\xd2\x8f\x57\x81\x76\x82\xcc\x81\x75\x95"
    "\x73\x97\x70\xcc\xda\xb0\xd1\x8d\xed\x8f\x9c\x81\x76\x82\xf0\x8e\xc0"
    "\x8d\x73\x82\xb5\x82\xc4\x89\xba\x82\xb3\x82\xa2";
const char kJpFrameRegTitle[] = "\xcc\xda\xb0\xd1\x93\x6f\x98\x5e";

// 0x4A4C22: overflow box; the limit is the pool's runtime capacity, which is
// the original 600000 (x86 300000) until a caller grows the pool.
void OverflowBoxBoneKey(unsigned char* m) {
    const int capacity =
        static_cast<int>(mikudancestudio::mdl::BoneKeyCapacity(
            *mikudancestudio::mdl::Mdl(m)));
    char text[256];
    if (mikudancestudio::mdl::Mdl(m)->physicsFlags != 0)
        sprintf_s(text, 0x100,
                  "You cannot regist over %d point\n"
                  "Please execute 'delete unused frame'",
                  capacity);
    else
        sprintf_s(text, 0x100, kJpOverflow,
                  capacity);
    // Caption 0x4A4C22: JP 0x52B908 when the JP flag is clear, "register
    // frame" when set (the JP constant was previously unused).
    MessageBoxA(*reinterpret_cast<HWND*>(m), text,
                mikudancestudio::mdl::Mdl(m)->physicsFlags != 0
                    ? "register frame"
                    : kJpFrameRegTitle,
                0);
}

// 60-byte bone-key record body write shared by all insert/overwrite paths
// of 0x4A4A50.  `p` points at the current 7-float source window inside the
// model+8620 pose array (window = p[0..6], 77-float stride per frame).
//   mode 0: position only   (quat = identity)
//   mode 1: rotation only   (pos = 0)
//   mode 2: full pose
//   other : nothing but the frame/links (original falls through the switch
//           without field writes or the mark)
void WriteBoneKeyRecord(mdl::BoneKey& rec, const float* p, int mode) {
    // 0x4A4BA9 / 0x4A4CAC / 0x4A527C
    if (mode == 0) {
        rec.position[0] = p[0]; rec.position[1] = p[1];
        rec.position[2] = p[2];
        rec.rotation[0] = rec.rotation[1] = rec.rotation[2] = 0.0f;
        rec.rotation[3] = 1.0f;
    } else if (mode == 1) {
        rec.position[0] = rec.position[1] = rec.position[2] = 0.0f;
        rec.rotation[3] = p[0]; rec.rotation[0] = p[1];
        rec.rotation[1] = p[2]; rec.rotation[2] = p[3];
    } else if (mode == 2) {
        rec.position[0] = p[0]; rec.position[1] = p[1];
        rec.position[2] = p[2];
        rec.rotation[3] = p[3]; rec.rotation[0] = p[4];
        rec.rotation[1] = p[5]; rec.rotation[2] = p[6];
    } else {
        return;
    }
    // linear interpolation control bytes {20,20,107,107} x4 (0x4A4CF1..)
    for (int g = 0; g < 4; ++g) {
        rec.interpolation[4 * g + 0] = 20;
        rec.interpolation[4 * g + 1] = 20;
        rec.interpolation[4 * g + 2] = 107;
        rec.interpolation[4 * g + 3] = 107;
    }
    rec.allocated = 1;
}

// Standard-bone SJIS name probes of 0x4A5690 (byte-exact, memcmp lengths).
const unsigned char kNameCenter[] =     // 0x531184 "センター" (9)
    {0x83, 0x5A, 0x83, 0x93, 0x83, 0x5E, 0x81, 0x5B, 0x00};
const unsigned char kNameUpper[] =      // 0x53117C "上半身" (7)
    {0x8F, 0xE3, 0x94, 0xBC, 0x90, 0x67, 0x00};
const unsigned char kNameNeck[] =      // 0x531178 "首" (3)
    {0x8E, 0xF1, 0x00};
const unsigned char kNameLArm[] =  // 0x52B81C "左腕" (5)
    {0x8D, 0xB6, 0x98, 0x72, 0x00};
const unsigned char kNameLElbow[] =     // 0x52B814 "左ひじ" (7)
    {0x8D, 0xB6, 0x82, 0xD0, 0x82, 0xB6, 0x00};
const unsigned char kNameRArm[] =  // 0x52B804 "右腕" (5)
    {0x89, 0x45, 0x98, 0x72, 0x00};
const unsigned char kNameRElbow[] =     // 0x52B7FC "右ひじ" (7)
    {0x89, 0x45, 0x82, 0xD0, 0x82, 0xB6, 0x00};
const unsigned char kNameLower[] =      // 0x531170 "下半身" (7)
    {0x89, 0xBA, 0x94, 0xBC, 0x90, 0x67, 0x00};
const unsigned char kNameLLeg[] =      // 0x531168 "左足" (5)
    {0x8D, 0xB6, 0x91, 0xAB, 0x00};
const unsigned char kNameRLeg[] =      // 0x531160 "右足" (5)
    {0x89, 0x45, 0x91, 0xAB, 0x00};
const unsigned char kNameLLegIk[] =     // 0x531154 "左足ＩＫ" (9)
    {0x8D, 0xB6, 0x91, 0xAB, 0x82, 0x68, 0x82, 0x6A, 0x00};
const unsigned char kNameRLegIk[] =     // 0x531148 "右足ＩＫ" (9)
    {0x89, 0x45, 0x91, 0xAB, 0x82, 0x68, 0x82, 0x6A, 0x00};
const unsigned char kNameLKnee[] =      // 0x530F60 "左ひざ" (7)
    {0x8D, 0xB6, 0x82, 0xD0, 0x82, 0xB4, 0x00};
const unsigned char kNameRKnee[] =      // 0x530F58 "右ひざ" (7)
    {0x89, 0x45, 0x82, 0xD0, 0x82, 0xB4, 0x00};
const unsigned char kNameLWrist[] =  // 0x52B80C "左手首" (7)
    {0x8D, 0xB6, 0x8E, 0xE8, 0x8E, 0xF1, 0x00};
const unsigned char kNameRWrist[] =  // 0x52B7F4 "右手首" (7)
    {0x89, 0x45, 0x8E, 0xE8, 0x8E, 0xF1, 0x00};
const unsigned char kNameLShoulder[] =      // 0x531140 "左肩" (5)
    {0x8D, 0xB6, 0x8C, 0xA8, 0x00};
const unsigned char kNameRShoulder[] =      // 0x531138 "右肩" (5)
    {0x89, 0x45, 0x8C, 0xA8, 0x00};

int FindBoneByName(unsigned char* m, const void* name, std::size_t cb) {
    const int cnt = mikudancestudio::mdl::Mdl(m)->boneCount;
    if (cnt <= 0) return -1;
    mikudancestudio::mdl::BoneRecord* const bones = mikudancestudio::mdl::Bones(m);
    for (int i = 0; i < cnt; ++i)
        if (std::memcmp(name, bones[i].name, cb) == 0) return i;
    return -1;  // original leaves the counter at cnt and skips the call
}

}  // namespace

// ---------------------------------------------------------------------------
// VA 0x004C2430 - ClearWaveContextFields(this) (was Sub4C2430): clear five fields of the 0x25C wave/
// timeline context object.  Called from the WinMain init path 0x47A5B0
// (0x47A665) right after the zeroing ctor 0x4C2450; the array pointers are
// nulled WITHOUT freeing (leak-preserving, matching the original).
// __thiscall(ctx) -> returns ctx.
// ---------------------------------------------------------------------------
void* ClearWaveContextFields(void* obj) {
    auto* audio = static_cast<WaveAudioContext*>(obj);
    audio->path[0] = L'\0';                              // 0x4C2434
    audio->englishUI = 0;                                // 0x4C2438
    audio->waveformMax = nullptr;                        // 0x4C243E
    audio->waveformMin = nullptr;                        // 0x4C2440
    audio->volume = 0;                                   // 0x4C2443
    return audio;                                        // 0x4C2449
}

// ---------------------------------------------------------------------------
// VA 0x004A4A50 - RegisterPhysicsPoseChain(model, boneIdx, srcIdx, mode,
// startFrame) (was Sub4A4A50).
// Physics-pose key registrar: walks the per-bone sorted chain of 60-byte
// records from record `boneIdx` and registers model+14584 consecutive
// frames (startFrame, startFrame+1, ...), sourcing each frame's pose from
// the model+8620 array (77-float stride, 7-float window per frame at float
// index srcIdx + 77*k).  Returns 0 on record exhaustion (box shown), 1 on
// success.  Uses the same AppendBoneKeyToUndo selection-unlink hook as the VMD
// registrars.  Sole caller: 0x4A5690.
// ---------------------------------------------------------------------------
bool RegisterPhysicsPoseChain(unsigned char* m, int boneIdx, int srcIdx,
                              int mode, std::uint32_t startFrame) {
    // Non-const: a free-slot scan below may grow the pool and move it, so the
    // pointer and the capacity are refreshed together.
    mdl::BoneKey* keys = mdl::BoneKeys(m);
    std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(m));

    // Grows the pool when a scan reached the end of the current block.  A false
    // return means the caller reports the overflow exactly as it did at the
    // fixed pool end.
    const auto growPool = [&]() -> bool {
        const std::size_t capacity = mdl::BoneKeyCapacity(*mdl::Mdl(m));
        if (!mdl::BoneKeyPoolEnsure(*mdl::Mdl(m), capacity + 1)) return false;
        keys = mdl::BoneKeys(m);
        boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(m));
        return true;
    };

    // chain walk: advance while frame < startFrame (unsigned, 0x4A4A8F..)
    int cur = boneIdx;                                            // v18
    if (keys[cur].frame < startFrame) {
        for (;;) {
            const int nxt = static_cast<int>(keys[cur].next);
            if (nxt == 0) break;                                  // 0x4A4A97
            if (keys[nxt].frame >= startFrame) break;
            cur = nxt;                                            // 0x4A4AAE
        }
    }

    int next = static_cast<int>(keys[cur].next);
    int scan = mikudancestudio::mdl::Mdl(m)->boneCount;                  // v9
    int cand = scan;                                              // v20
    if (static_cast<std::size_t>(scan) >= boneKeyCapacity) {
        if (!growPool()) {
            OverflowBoxBoneKey(m);
            return false;
        }
    }
    if (keys[scan].frame != 0) {
        do {
            ++scan;
            if (static_cast<std::size_t>(scan) >= boneKeyCapacity) {  // 0x4A4AFC
                if (!growPool()) {
                    OverflowBoxBoneKey(m);                            // 0x4A4C22
                    return false;
                }
            }
        } while (keys[scan].frame != 0);
        cand = scan;                                              // 0x4A4B0E
    }

    if (mikudancestudio::mdl::Mdl(m)->matMisc == 0) return true;          // 0x4A4B22

    std::uint32_t frame = startFrame;                             // v19
    const float* p = static_cast<const float*>(
                          mikudancestudio::mdl::PoseTraceBuffer(m)) +
                     srcIdx;              // i = srcIdx (measured in floats; advances by 77 per sample). Defect FB-08.
    unsigned int iter = 0;                                        // v22
    for (;;) {
        AppendBoneKeyToUndo(m, cur);                                        // 0x4A4B47
        AppendBoneKeyToUndo(m, next);                                       // 0x4A4B53
        AppendBoneKeyToUndo(m, scan);                                       // 0x4A4B5B

        if (keys[cur].frame == frame) {
            // exact-frame overwrite at cur (unlink, rewrite, relink below)
            keys[cur].next = 0;
            WriteBoneKeyRecord(keys[cur], p, mode);
        } else {
            // insert path (0x4A4F00)
            if (keys[next].frame == frame) {
                // overwrite the successor record instead (0x4A4F05..)
                keys[cur].next = static_cast<std::uint32_t>(next);
                keys[next].previous = static_cast<std::uint32_t>(cur);
                WriteBoneKeyRecord(keys[next], p, mode);
                cur = next;                                       // 0x4A520A
                scan = cand;                                      // 0x4A5218
                next = static_cast<int>(keys[cur].next);
            } else {
                // fresh record at cand (0x4A5229..)
                keys[cur].next = static_cast<std::uint32_t>(cand);
                keys[cand].previous = static_cast<std::uint32_t>(cur);
                keys[cand].frame = frame;
                keys[cand].next = 0;
                WriteBoneKeyRecord(keys[cand], p, mode);
                cur = cand;                                       // 0x4A5550
                scan = cand + 1;                                  // 0x4A5554
                cand = scan;
                if (static_cast<std::size_t>(scan) >= boneKeyCapacity) {
                    if (!growPool()) {
                        OverflowBoxBoneKey(m);                   // 0x4A5636
                        return false;                             // 0x4A55FB
                    }
                }
                if (keys[scan].frame != 0) {
                    for (;;) {
                        ++scan;
                        cand = scan;                              // 0x4A5578
                        if (static_cast<std::size_t>(scan) >= boneKeyCapacity) {  // 0x4A557C
                            if (!growPool()) {
                                OverflowBoxBoneKey(m);            // 0x4A5636
                                return false;                     // 0x4A55FB
                            }
                        }
                        if (keys[scan].frame == 0) break;
                    }
                }
            }
        }
        // LABEL_42 (0x4A5598)
        if (frame > mikudancestudio::mdl::Mdl(m)->maxFrame)
            mikudancestudio::mdl::Mdl(m)->maxFrame = frame;
        if (next != 0) {                                           // 0x4A55A6
            keys[cur].next = static_cast<std::uint32_t>(next);
            keys[next].previous = static_cast<std::uint32_t>(cur);
        }
        ++frame;                                                   // 0x4A55D6
        ++iter;
        p += 77;                                                   // i += 308
        if (iter >= static_cast<unsigned int>(
                        mikudancestudio::mdl::Mdl(m)->matMisc))
            return true;                                          // 0x4A55EE
    }
}

// ---------------------------------------------------------------------------
// VA 0x004A5690 - RegisterPhysicsPose(model, frame) (was Sub4A5690): the
// "register physics-driven
// pose" driver.  Clears every mark byte in the three key arrays, snapshots
// the current pose into the undo ring (slot = model+12724, 30-deep), then
// probes the standard skeleton by SJIS bone name and runs RegisterPhysicsPoseChain for
// each found chain.  On any registrar failure it returns EARLY, skipping
// the model+8620 free and the model+14584 reset (original leak/quirk).
// __thiscall(model).  Sole caller: 0x46B090.
// ---------------------------------------------------------------------------
void RegisterPhysicsPose(unsigned char* m, std::uint32_t frame) {
    mdl::ModelRecord& model = *mdl::Mdl(m);
    mdl::BoneKey* const boneKeys = mdl::BoneKeys(m);
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(model);
    for (std::size_t i = 0; i < boneKeyCapacity; ++i)
        boneKeys[i].allocated = 0;
    mdl::MorphKey* const morphKeys = mdl::MorphKeys(m);
    const auto morphCapacity1 = mdl::MorphKeyCapacity(morphKeys);
    for (int i = 0; i < morphCapacity1; ++i) morphKeys[i].allocated = 0;
    mdl::DisplayKey* const masterKeys = mdl::DisplayKeys(m);
    for (int i = 0, poolCapacity = static_cast<int>(mdl::KeyPoolCapacity(masterKeys)); i < poolCapacity; ++i) masterKeys[i].allocated = 0;

    // Defect FB-13: the pose-trace buffer and the undo ring's pose buffers are
    // allocated via the scalar ::operator new (dialog_gaps.cpp:590 at
    // 0x4B57B0, and 0x4A57F1/0x4A5995 in this file), so they must be released
    // with the scalar ::operator delete. A CRT free() on storage from ::operator new is
    // undefined behaviour, even though the MSVC heap masks it; the app side
    // (oni_skeleton_pump.cpp) freed the same buffers with delete[], which is
    // the same mismatch in the opposite direction.
    const auto cleanup = [&]() {                                   // 0x4A60BF
        if (mikudancestudio::mdl::PoseTraceBuffer(m) != nullptr) {
            ::operator delete(mikudancestudio::mdl::PoseTraceBuffer(m));
            mikudancestudio::mdl::PoseTraceBuffer(m) = nullptr;
        }
        model.matMisc = 0;
    };

    if (model.matMisc == 0) {                                    // 0x4A56FC
        cleanup();
        return;
    }

    HWND hwnd = static_cast<HWND>(model.hwnd);
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), 1);                        // 0x4A5713
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), 0);                        // 0x4A572A

    if (++model.undoState[0] >= 30)
        model.undoState[0] = 0;                                   // 0x4A574D
    const int slot = model.undoState[0];
    model.undoState[1] = slot;                                    // 0x4A5762
    bone_scale::CaptureUndo(m);
    model.undoDirty = 1;                                          // 0x4A573D
    model.redoDirty = 0;                                          // 0x4A5744

    auto& undo = mikudancestudio::mdl::Mdl(m)->undoRings[0].slots[slot];
    undo.operation = 2;                                            // 0x4A5768
    undo.frame = frame;                                             // 0x4A5786

    const int boneCnt = model.boneCount;
    auto*& pose = undo.bonePose;
    if (pose != nullptr) {                                         // 0x4A579C
        ::operator delete(pose);
        pose = nullptr;
    }
    pose = static_cast<mikudancestudio::mdl::BonePoseSnapshot*>(
        ::operator new(sizeof(*pose) * boneCnt));                    // 0x4A57F1
    std::memset(pose, 0, sizeof(*pose) * static_cast<std::size_t>(boneCnt));
    if (boneCnt > 0) {                                             // 0x4A582C
        mikudancestudio::mdl::BoneRecord* const bones = model.boneTable;
        const unsigned char* flags = model.bonePhysicsState;
        for (int b = 0; b < boneCnt; ++b) {
            auto& rec = pose[b];
            const mikudancestudio::mdl::BoneRecord* bone = &bones[b];
            rec.boneIndex = b;                                    // 0x4A5856
            std::memcpy(rec.position, bone->trans, sizeof(rec.position));
            std::memcpy(rec.rotation, bone->rotQuat, sizeof(rec.rotation));
            rec.physicsDisabled = flags[b];                        // 0x4A58F2
        }
    }
    undo.dirty = 0;                                                 // 0x4A5922

    auto*& poseSrc = undo.auxiliaryPose;
    if (poseSrc != nullptr) {                                      // 0x4A5939
        ::operator delete(poseSrc);
        poseSrc = nullptr;
    }
    poseSrc = static_cast<unsigned char*>(
        ::operator new(3456 * static_cast<std::size_t>(model.matMisc))); // 0x4A5995
    std::memset(poseSrc, 0,
                3456 * static_cast<std::size_t>(model.matMisc));   // 0x4A59C2
    mdl::BoneKeyVisitClear(*mdl::Mdl(m));               // 0x4A59D5

    // standard-bone probes (order and srcIdx/mode pairs from the original)
    int i;
    i = FindBoneByName(m, kNameCenter, 9);                         // 0x4A5A00
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 0, 0, frame)) return;          // 0x4A5A2D
    i = FindBoneByName(m, kNameUpper, 7);                          // 0x4A5A53
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 3, 1, frame)) return;          // 0x4A5A80
    if (model.openniVersion >= 14) {                               // 0x4A5A8D
        i = FindBoneByName(m, kNameNeck, 3);                      // 0x4A5AB0
        if (i != -1 && !RegisterPhysicsPoseChain(m, i, 7, 1, frame)) return;      // 0x4A5ADD
    }
    i = FindBoneByName(m, kNameLArm, 5);                      // 0x4A5B03
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 11, 1, frame)) return;         // 0x4A5B30
    i = FindBoneByName(m, kNameLElbow, 7);                         // 0x4A5B60
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 15, 1, frame)) return;         // 0x4A5B8D
    i = FindBoneByName(m, kNameRArm, 5);                      // 0x4A5BB3
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 19, 1, frame)) return;         // 0x4A5BE0
    i = FindBoneByName(m, kNameRElbow, 7);                         // 0x4A5C10
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 23, 1, frame)) return;         // 0x4A5C3D
    i = FindBoneByName(m, kNameLower, 7);                          // 0x4A5C63
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 27, 1, frame)) return;         // 0x4A5C90
    i = FindBoneByName(m, kNameLLeg, 5);                          // 0x4A5CC0
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 31, 1, frame)) return;         // 0x4A5CED
    i = FindBoneByName(m, kNameRLeg, 5);                          // 0x4A5D13
    if (i != -1 && !RegisterPhysicsPoseChain(m, i, 35, 1, frame)) return;         // 0x4A5D40

    // wrist IK chains (model+9920, 24-byte structs: +0 root bone idx,
    // +18 enabled flag): enabled -> wrist bone pose, else the knee
    {
        const int ikCnt = model.ikChainCount;
        const mdl::IkChain* const chains = model.ikChains;
        mikudancestudio::mdl::BoneRecord* const bones = model.boneTable;
        int chain = -1;
        for (int c = 0; c < ikCnt; ++c) {                          // 0x4A5D80
            const mikudancestudio::mdl::BoneRecord* root = &bones[chains[c].boneIndex];
            if (std::memcmp(kNameLLegIk, root->name, 9) == 0) { chain = c; break; }
        }
        if (chain >= 0) {
            if (chains[chain].enabled != 0) {                      // 0x4A5A9D
                i = FindBoneByName(m, kNameLLegIk, 9);             // 0x4A5DD0
                if (i != -1 && !RegisterPhysicsPoseChain(m, i, 47, 2, frame)) return; // 0x4A5DF3
            } else {
                i = FindBoneByName(m, kNameLKnee, 7);              // 0x4A5E14
                if (i != -1 && !RegisterPhysicsPoseChain(m, i, 39, 1, frame)) return; // 0x4A5E3A
            }
        }
    }
    {
        const int ikCnt = model.ikChainCount;
        const mdl::IkChain* const chains = model.ikChains;
        mikudancestudio::mdl::BoneRecord* const bones = model.boneTable;
        int chain = -1;
        for (int c = 0; c < ikCnt; ++c) {                          // 0x4A5E80
            const mikudancestudio::mdl::BoneRecord* root = &bones[chains[c].boneIndex];
            if (std::memcmp(kNameRLegIk, root->name, 9) == 0) { chain = c; break; }
        }
        if (chain >= 0) {
            if (chains[chain].enabled != 0) {                      // 0x4A5E9D
                i = FindBoneByName(m, kNameRLegIk, 9);             // 0x4A5ED0
                if (i != -1 && !RegisterPhysicsPoseChain(m, i, 54, 2, frame)) return; // 0x4A5EF3
            } else {
                i = FindBoneByName(m, kNameRKnee, 7);              // 0x4A5F14
                if (i != -1 && !RegisterPhysicsPoseChain(m, i, 43, 1, frame)) return; // 0x4A5F3A
            }
        }
    }

    if (model.openniVersion >= 14) {                               // 0x4A5F4E
        i = FindBoneByName(m, kNameLWrist, 7);                  // 0x4A5F74
        if (i != -1 && !RegisterPhysicsPoseChain(m, i, 61, 1, frame)) return;     // 0x4A5FA1
        // 0x4A5FAE is a REAL re-test of the spec gate in the binary (cmp
        // byte ptr [ebp+38FDh], 0Eh / jl), not decompiler noise.  Nothing
        // in FindBoneByName/RegisterPhysicsPoseChain writes model.openniVersion, so the re-test
        // always re-enters here; kept verbatim for structural fidelity.
        if (model.openniVersion >= 14) {                           // 0x4A5FAE
            i = FindBoneByName(m, kNameRWrist, 7);              // 0x4A5FD0
            if (i != -1 && !RegisterPhysicsPoseChain(m, i, 65, 1, frame)) return; // 0x4A5FFD
        }
    }
    if (model.openniVersion >= 15) {                               // 0x4A600A
        i = FindBoneByName(m, kNameLShoulder, 5);                      // 0x4A6030
        // 0x4A6056/0x4A60B6: like every other probe, a LFoot/RFoot
        // failure is a PLAIN return in the original - no cleanup.
        if (i != -1 && !RegisterPhysicsPoseChain(m, i, 69, 1, frame))             // 0x4A6056
            return;
        // 0x4A606A: same real re-test in the binary (cmp byte ptr
        // [ebp+38FDh], 0Fh / jl); always true here, kept verbatim.
        if (model.openniVersion >= 15) {                           // 0x4A606A
            i = FindBoneByName(m, kNameRShoulder, 5);                  // 0x4A6090
            if (i != -1 && !RegisterPhysicsPoseChain(m, i, 73, 1, frame))         // 0x4A60B6
                return;
        }
    }
    cleanup();                                                     // 0x4A60BF
}

// ---------------------------------------------------------------------------
// VA 0x004A60F0 - BuildLookAtQuaternion(out, quat, ax, ay, az, bx, by, bz,
// mode) (was Sub4A60F0):
// look-at quaternion builder.  Rotates the (b - a) direction by
// inverse(quat) * RotY(pi), normalizes it (mode 5 zeroes Y first), then
// constructs a yaw/roll rotation for the chosen axis mode (0..5) and
// returns the quaternion with its Y component negated.
// __stdcall; `out` is the return value (4 floats).  Sole caller: 0x4A6520
// (10 call sites).
// ---------------------------------------------------------------------------
float* BuildLookAtQuaternion(float out[4], const float quat[4], float ax,
                             float ay, float az, float bx, float by, float bz,
                             unsigned char mode) {
    auto* d3 = &d3dx::Get();
    FnQuatInverseLocal quatInverse = LocalQuatInverse();
    if (d3->module == nullptr || quatInverse == nullptr ||
        d3->matrixRotationQuaternion == nullptr || d3->rotY == nullptr ||
        d3->multiply == nullptr || d3->vec3Transform == nullptr ||
        d3->vec3Normalize == nullptr || d3->quatFromMatrix == nullptr)
        return IdentityQuaternion(out);

    const double quaternionLength = double(quat[0]) * quat[0] + double(quat[1]) * quat[1] +
        double(quat[2]) * quat[2] + double(quat[3]) * quat[3];
    if (!std::isfinite(quaternionLength) || quaternionLength == 0.0)
        return IdentityQuaternion(out);

    float qInv[4];
    quatInverse(qInv, quat);                                       // 0x4A6109
    d3dx::D3DXMATRIXF m1;
    d3->matrixRotationQuaternion(&m1, qInv);                       // 0x4A611B
    d3dx::D3DXMATRIXF m2;
    d3->rotY(&m2, kPi52B738);                                      // 0x4A612F
    d3dx::D3DXMATRIXF m3;
    d3->multiply(&m3, &m1, &m2);                                   // 0x4A6146

    float dir[3] = {bx - ax, by - ay, bz - az};                    // 0x4A616C..
    float t[4];
    d3->vec3Transform(t, dir, &m3);                                // 0x4A61C3

    float n[3] = {t[0], mode == 5 ? 0.0f : t[1], t[2]};            // 0x4A61D9..
    if (!NormalizeDirection(n)) return IdentityQuaternion(out);    // 0x4A6201

    const float len = sqrtf(n[2] * n[2] + n[0] * n[0]);            // 0x4A6216 (v46)
    float c;   // v40
    float s;   // v44
    float ang = 0.0f;  // v42

    if (len <= 1.0e-7f) {
        // A vertical direction has no yaw. Keep a deterministic zero yaw,
        // while the following pitch matrix still uses its vertical component.
        c = 1.0f;
        s = 0.0f;
    } else if (mode == 5) {
        c = -n[2] / len;                                           // 0x4A6244
        s = n[0] / len;                                            // 0x4A624C
    } else {
        c = n[0] / len;                                            // 0x4A6266
        s = n[2] / len;                                            // 0x4A6270
    }
    if (mode == 1) {                                           // 0x4A6274
        const float a = acosf(ClampUnit(c));                   // 0x4A6285
        ang = s < 0.0f ? -a : a;   // v15 = 0, v14 = v46 kept
    } else if (mode >= 2) {        // mode 0: v15 = 0         // 0x4A6316
        // modes 2/3/4/5: two-term angle decomposition with the x87
        // flag-sensitive sign selection (fnstsw parity + C0/C3 tests at
        // 0x4A633F..0x4A6349 map to ordered "< 0" tests; NaN falls to the
        // ">= 0" arm).
        const float ang0 = acosf(ClampUnit(c));                    // 0x4A6323
        float r;
        if (s < 0.0f) {
            r = !(c < 0.0f) ? -acosf(ClampUnit(-s)) : acosf(ClampUnit(-s)); // 0x4A634D/5C
            ang = -ang0 - r * len;                                 // 0x4A6385
        } else {
            r = !(c < 0.0f) ? -acosf(ClampUnit(s)) : acosf(ClampUnit(s)); // 0x4A6395/A4
            ang = ang0 - r * len;                                  // 0x4A63C9
        }
    }
    {
        // first 2D rotation from (c, s) (0x4A62AE..)
        d3dx::D3DXMATRIXF& a = m1;  // reused v53
        std::memset(&a, 0, sizeof(a));
        a.m[0][0] = c;                                             // 0x4A62E6
        a.m[0][2] = -s;                                            // 0x4A62F2
        a.m[2][0] = s;                                             // 0x4A62F6
        a.m[2][2] = c;                                             // 0x4A62FA
        a.m[1][1] = 1.0f;                                          // 0x4A62DE
        a.m[3][3] = 1.0f;                                          // 0x4A62DA
    }
    float e1, e2;   // v41, v45
    if (mode != 0) {                                               // 0x4A62FE
        e1 = -n[1];                                                // 0x4A63DA
        e2 = len;                                                  // 0x4A63DE
    } else {
        e1 = len;                                                  // 0x4A6304
        e2 = n[1];                                                 // 0x4A630A
    }
    {
        d3dx::D3DXMATRIXF& b = m2;  // reused v54
        std::memset(&b, 0, sizeof(b));
        b.m[0][0] = e1;                                            // 0x4A643F
        b.m[0][1] = -e2;                                           // 0x4A644B
        b.m[1][0] = e2;                                            // 0x4A644F
        b.m[1][1] = e1;                                            // 0x4A6456
        b.m[2][2] = 1.0f;                                          // 0x4A6434
        b.m[3][3] = 1.0f;                                          // 0x4A642D
    }
    d3dx::D3DXMATRIXF r;   // v57
    if (mode < 4) {                                                // 0x4A645D
        d3->multiply(&m3, &m1, &m2);                               // 0x4A6477
        std::memcpy(&r, &m3, sizeof(r));                           // 0x4A648F
    } else {
        std::memcpy(&r, &m1, sizeof(r));                           // 0x4A645F
    }
    if (mode == 1 || mode == 2 || mode == 3) {                     // 0x4A6493
        d3->rotY(&m1, -ang);                                       // 0x4A64BE
        d3->multiply(&m3, &r, &m1);                                // 0x4A64D8
        std::memcpy(&r, &m3, sizeof(r));                           // 0x4A64F0
    }
    d3->quatFromMatrix(out, &r);                                   // 0x4A6502
    out[1] = -out[1];                                              // 0x4A650D
    return out;                                                    // 0x4A6512
}

// ---------------------------------------------------------------------------
// VA 0x004A6520 - InitStandardSkeletonQuats(model, flag) (was Sub4A6520):
// standard-skeleton base-quaternion
// initializer.  Using the standard-bone probe positions cached at
// model+14284..14568 (-999.0 = absent), builds the rest orientation
// quaternions for the upper body (model+64), neck (+80), arm chains
// (+96..+188), lower body (+192) and leg chains (+208..+300), plus the two
// shoulder mid rotations (+304/+320) and the leg length cache (+14568).
// `flag` (a2): when a probe group is absent, initialize that quaternion to
// identity instead of leaving it (some blocks do this unconditionally -
// preserved).  Original returns an undefined eax; ported as void.
// __thiscall(model).  Sole caller: 0x4B5760.
// ---------------------------------------------------------------------------
void InitStandardSkeletonQuats(unsigned char* m, unsigned char flag) {
    auto* d3 = &d3dx::Get();
    FnQuatRotationAxisLocal quatRotationAxis = LocalQuatRotationAxis();
    if (d3->module == nullptr || quatRotationAxis == nullptr ||
        d3->vec3Normalize == nullptr || d3->quatMultiply == nullptr)
        return;

    auto& model = *mdl::Mdl(m);
    float qByVal[4];  // by-value quaternion reads (original passes 4 dwords)
    // The arm twist blocks rotate about (0, 0, 1) - the v139/v140 register
    // trio always carries (0, 0, 1) at those call sites.
    const float kAxisZ[3] = {0.0f, 0.0f, 1.0f};

    // scratch quats (v144/v148/v132-style stack temps)
    float qA[4], qB[4], t1[4], t2[4];

    // q150/q155/q159 persist across blocks; the original leaves them as
    // uninitialized stack when producer blocks are skipped. An unavailable
    // direction contributes no rotation to the subsequent quaternion product.
    float q150[4] = {0,0,0,1}, q155[4] = {0,0,0,1}, q159[4] = {0,0,0,1};

    // ---- leg length cache (0x4A6543..0x4A6814) ---------------------------
    if (model.lightDir[0] == -1.0f) {
        if (model.standardBonePositions[13][1] != kSentinel && model.standardBonePositions[14][1] != kSentinel &&
            model.standardBonePositions[12][1] != kSentinel) {
            const float d1x = model.standardBonePositions[14][0] - model.standardBonePositions[13][0];
            const float d1y = model.standardBonePositions[14][1] - model.standardBonePositions[13][1];
            const float d1z = model.standardBonePositions[14][2] - model.standardBonePositions[13][2];
            const float d2x = model.standardBonePositions[13][0] - model.standardBonePositions[12][0];
            const float d2y = model.standardBonePositions[13][1] - model.standardBonePositions[12][1];
            const float d2z = model.standardBonePositions[13][2] - model.standardBonePositions[12][2];
            const float len1 = sqrtf(d1y * d1y + d1x * d1x + d1z * d1z); // 0x4A6661
            const float len2 = sqrtf(d2x * d2x + d2y * d2y + d2z * d2z); // 0x4A6690
            model.lightDir[0] = len2 + len1;                         // 0x4A680E
        } else if (model.standardBonePositions[13][1] != kSentinel && model.standardBonePositions[18][1] != kSentinel &&
                   model.standardBonePositions[16][1] != kSentinel) {
            const float d1x = model.standardBonePositions[18][0] - model.standardBonePositions[17][0];
            const float d1y = model.standardBonePositions[18][1] - model.standardBonePositions[17][1];
            const float d1z = model.standardBonePositions[18][2] - model.standardBonePositions[17][2];
            const float d2x = model.standardBonePositions[17][0] - model.standardBonePositions[16][0];
            const float d2y = model.standardBonePositions[17][1] - model.standardBonePositions[16][1];
            const float d2z = model.standardBonePositions[17][2] - model.standardBonePositions[16][2];
            const float lenA = sqrtf(d1y * d1y + d1x * d1x + d1z * d1z); // 0x4A67C6
            const float lenB = sqrtf(d2x * d2x + d2y * d2y + d2z * d2z); // 0x4A67F5
            model.lightDir[0] = lenB + lenA;                         // 0x4A680E
        }
    }

    // ---- head / upper-body base quat, model+64 (0x4A6827..0x4A6A5E) ------
    if (model.standardBonePositions[1][1] != kSentinel && model.standardBonePositions[4][1] != kSentinel &&
        model.standardBonePositions[8][1] != kSentinel && model.standardBonePositions[2][1] != kSentinel) {
        float dx = model.standardBonePositions[2][0] - model.standardBonePositions[1][0];                            // 0x4A68AB
        float dy = model.standardBonePositions[2][1] - model.standardBonePositions[1][1];                            // 0x4A68C8
        float dz = model.standardBonePositions[2][2] - model.standardBonePositions[1][2];                            // 0x4A68E3
        NormalizeCoordinates(dx, dy, dz);                               // 0x4A68EF
        float cx = dz * 1.0f - dy * 0.0f;                          // 0x4A6914
        float cy = dx * 0.0f - dz * 0.0f;                          // 0x4A6935
        float cz = dy * 0.0f - dx * 1.0f;                          // 0x4A6950
        const float dot =
            1.0f * dy + dx * 0.0f + 0.0f * dz;                     // 0x4A696C
        float axis[3] = {cx, cy, cz};
        RotationAxisFromDot(qA, axis, dot, 0.0f, 1.0f, 0.0f);       // 0x4A6994
        std::memcpy(qByVal, model.localTransforms + 0, sizeof(qByVal));
        BuildLookAtQuaternion(q150, qByVal, model.standardBonePositions[8][0], model.standardBonePositions[8][1], model.standardBonePositions[8][2],
                  model.standardBonePositions[4][0], model.standardBonePositions[4][1], model.standardBonePositions[4][2], 4);                // 0x4A6A06
        std::memcpy((model.localTransforms + 0), q150, 16);                           // 0x4A6A0D..
        d3->quatMultiply(t1, (model.localTransforms + 0), qA);                        // 0x4A6A30
        std::memcpy((model.localTransforms + 0), t1, 16);                             // 0x4A6A4B..
    } else if (flag != 0) {
        model.localTransforms[3] = 1.0f;                                       // 0x4A6A60
        model.localTransforms[0] = 0.0f;                                       // 0x4A6A65
        model.localTransforms[1] = 0.0f;                                       // 0x4A6A68
        model.localTransforms[2] = 0.0f;                                       // 0x4A6A6B
    }

    // ---- neck quat, model+80 (0x4A6A7F..0x4A6C60) ------------------------
    if (model.standardBonePositions[0][1] != kSentinel && model.standardBonePositions[3][1] != kSentinel &&
        model.standardBonePositions[2][1] != kSentinel) {
        std::memcpy(qByVal, model.localTransforms + 0, sizeof(qByVal));
        BuildLookAtQuaternion(t1, qByVal, model.standardBonePositions[3][0], model.standardBonePositions[3][1], model.standardBonePositions[3][2],
                  model.standardBonePositions[2][0], model.standardBonePositions[2][1], model.standardBonePositions[2][2], 1);                // 0x4A6B33
        std::memcpy((model.localTransforms + 4), t1, 16);                             // 0x4A6B4B..
        if (model.standardBonePositions[22][1] != kSentinel) {                               // 0x4A6B49
            d3->quatMultiply(qB, (model.localTransforms + 4), (model.localTransforms + 0));              // 0x4A6B75
            BuildLookAtQuaternion(qA, qB, model.standardBonePositions[22][0], model.standardBonePositions[22][1], model.standardBonePositions[22][2],
                      model.standardBonePositions[3][0], model.standardBonePositions[3][1], model.standardBonePositions[3][2], 5);            // 0x4A6BE4
            d3->quatMultiply(t1, qA, (model.localTransforms + 4));                    // 0x4A6C0F
            std::memcpy((model.localTransforms + 4), t1, 16);                         // 0x4A6C2C..
        }
    } else {
        // unconditional identity (v32/v36 are constant 0/1 here)
        model.localTransforms[7] = 1.0f;                                       // 0x4A6C55
        model.localTransforms[4] = 0.0f;                                       // 0x4A6C5A
        model.localTransforms[5] = 0.0f;                                       // 0x4A6C5D
        model.localTransforms[6] = 0.0f;                                       // 0x4A6C60
    }

    // ---- shoulder mid quat #1, model+320 (0x4A6C4A..0x4A6E2C) -----------
    if (model.standardBonePositions[8][1] != kSentinel && model.standardBonePositions[20][1] != kSentinel &&
        model.standardBonePositions[4][1] != kSentinel) {
        const float mx = (model.standardBonePositions[4][0] + model.standardBonePositions[8][0]) * 0.5f;             // 0x4A6CE9
        const float my = (model.standardBonePositions[8][1] + model.standardBonePositions[4][1]) * 0.5f;             // 0x4A6CCF..
        const float mz = (model.standardBonePositions[8][2] + model.standardBonePositions[4][2]) * 0.5f;             // 0x4A6CFB
        float ax = model.standardBonePositions[4][0] - mx, ay = model.standardBonePositions[4][1] - my,
              az = model.standardBonePositions[4][2] - mz;                                  // 0x4A6D0F..
        float bx = model.standardBonePositions[20][0] - mx, by = model.standardBonePositions[20][1] - my,
              bz = model.standardBonePositions[20][2] - mz;                                  // 0x4A6D5D..
        NormalizeCoordinates(ax, ay, az);                               // 0x4A6D95
        NormalizeCoordinates(bx, by, bz);                               // 0x4A6DA2
        float cx = bz * ay - by * az;                              // 0x4A6DC7
        float cy = bx * az - bz * ax;                              // 0x4A6DE8
        float cz = by * ax - bx * ay;                              // 0x4A6E03
        const float dot = ay * by + bx * ax + az * bz;             // 0x4A6E1F
        float axis[3] = {cx, cy, cz};
        RotationAxisFromDot((model.localTransforms + 64), axis, dot, ax, ay, az);             // 0x4A6E49
    } else {
        model.localTransforms[67] = 1.0f;                                      // 0x4A6E64
        model.localTransforms[64] = 0.0f;                                      // 0x4A6E6C
        model.localTransforms[65] = 0.0f;                                      // 0x4A6E6F
        model.localTransforms[66] = 0.0f;                                      // 0x4A6E75
    }

    // ---- upper arm quat, model+144 (0x4A6E9C..0x4A7079) ------------------
    if (model.standardBonePositions[4][1] != kSentinel && model.standardBonePositions[5][1] != kSentinel &&
        model.standardBonePositions[2][1] != kSentinel) {
        quatRotationAxis(qA, kAxisZ, -k35deg5311A4);               // 0x4A6EFB
        if (model.standardBonePositions[20][1] == kSentinel) {                               // 0x4A6F11
            std::memcpy(qByVal, model.localTransforms + 0, sizeof(qByVal));
            BuildLookAtQuaternion(q150, qByVal, model.standardBonePositions[4][0], model.standardBonePositions[4][1], model.standardBonePositions[4][2],
                      model.standardBonePositions[5][0], model.standardBonePositions[5][1], model.standardBonePositions[5][2], 0);            // 0x4A6FF2
        } else {
            d3->quatMultiply(qB, (model.localTransforms + 64), (model.localTransforms + 0));             // 0x4A6F1D
            BuildLookAtQuaternion(q150, qB, model.standardBonePositions[20][0], model.standardBonePositions[20][1], model.standardBonePositions[20][2],
                      model.standardBonePositions[5][0], model.standardBonePositions[5][1], model.standardBonePositions[5][2], 0);            // 0x4A6F8A
        }
        d3->quatMultiply((model.localTransforms + 20), qA, q150);                     // 0x4A7023
    } else if (flag != 0) {                                        // 0x4A702C
        quatRotationAxis(qA, kAxisZ, k35deg5311A4);                // 0x4A7048
        std::memcpy((model.localTransforms + 20), qA, 16);                            // 0x4A7059..
    }

    // ---- forearm quat, model+176 (0x4A7090..0x4A7211) --------------------
    if (model.standardBonePositions[5][1] != kSentinel && model.standardBonePositions[6][1] != kSentinel &&
        model.standardBonePositions[4][1] != kSentinel) {
        quatRotationAxis(qA, kAxisZ, -k30deg53119C);               // 0x4A70EF
        d3->quatMultiply(t1, q150, (model.localTransforms + 64));                     // 0x4A70FF
        d3->quatMultiply(t2, t1, (model.localTransforms + 0));                        // 0x4A7112
        BuildLookAtQuaternion(q155, t2, model.standardBonePositions[5][0], model.standardBonePositions[5][1], model.standardBonePositions[5][2],
                  model.standardBonePositions[6][0], model.standardBonePositions[6][1], model.standardBonePositions[6][2], 0);                // 0x4A7181
        d3->quatMultiply((model.localTransforms + 28), qA, q155);                     // 0x4A71C1
        quatRotationAxis(qA, kAxisZ, k30deg53119C);                // 0x4A71DA
        d3->quatMultiply((model.localTransforms + 28), (model.localTransforms + 28), qA);                // 0x4A71E6
    } else if (flag != 0) {                                        // 0x4A7211
        model.localTransforms[31] = 1.0f;                                      // 0x4A7213
        model.localTransforms[28] = 0.0f;                                      // 0x4A721B
        model.localTransforms[29] = 0.0f;                                      // 0x4A7221
        model.localTransforms[30] = 0.0f;                                      // 0x4A7227
    }

    // ---- wrist quat, model+160 (0x4A7206..0x4A743E) ----------------------
    if (model.standardBonePositions[6][1] != kSentinel && model.standardBonePositions[7][1] != kSentinel &&
        model.standardBonePositions[5][1] != kSentinel) {
        if (model.standardBonePositions[7][0] == model.standardBonePositions[6][0] && model.standardBonePositions[7][1] == model.standardBonePositions[6][1] &&
            model.standardBonePositions[7][2] == model.standardBonePositions[6][2]) {                                // 0x4A72A8
            model.localTransforms[27] = 1.0f;                                  // 0x4A72AA
            model.localTransforms[24] = 0.0f;                                  // 0x4A72B2
            model.localTransforms[25] = 0.0f;                                  // 0x4A72B8
            model.localTransforms[26] = 0.0f;                                  // 0x4A72BE
        } else {
            quatRotationAxis(qA, kAxisZ, -k30deg53119C);           // 0x4A72E3
            d3->quatMultiply(t1, q155, q150);                      // 0x4A72FA
            d3->quatMultiply(t2, t1, (model.localTransforms + 64));                   // 0x4A730D
            d3->quatMultiply(qB, t2, (model.localTransforms + 0));                    // 0x4A7323
            BuildLookAtQuaternion(q159, qB, model.standardBonePositions[6][0], model.standardBonePositions[6][1], model.standardBonePositions[6][2],
                      model.standardBonePositions[7][0], model.standardBonePositions[7][1], model.standardBonePositions[7][2], 0);            // 0x4A7392
            d3->quatMultiply((model.localTransforms + 24), qA, q159);                 // 0x4A73D2
            quatRotationAxis(qA, kAxisZ, k30deg53119C);            // 0x4A73EB
            d3->quatMultiply((model.localTransforms + 24), (model.localTransforms + 24), qA);            // 0x4A73F7
        }
    } else if (flag != 0) {                                        // 0x4A7420
        model.localTransforms[27] = 1.0f;                                      // 0x4A7422
        model.localTransforms[24] = 0.0f;                                      // 0x4A742A
        model.localTransforms[25] = 0.0f;                                      // 0x4A7430
        model.localTransforms[26] = 0.0f;                                      // 0x4A7436
    }

    // ---- shoulder mid quat #2, model+304 (0x4A7415..0x4A7668) -----------
    if (model.standardBonePositions[8][1] != kSentinel && model.standardBonePositions[21][1] != kSentinel &&
        model.standardBonePositions[4][1] != kSentinel) {
        const float mx = (model.standardBonePositions[4][0] + model.standardBonePositions[8][0]) * 0.5f;             // 0x4A7490
        const float my = (model.standardBonePositions[8][1] + model.standardBonePositions[4][1]) * 0.5f;             // 0x4A74A0
        const float mz = (model.standardBonePositions[8][2] + model.standardBonePositions[4][2]) * 0.5f;             // 0x4A74B0
        float ax = model.standardBonePositions[8][0] - mx, ay = model.standardBonePositions[8][1] - my,
              az = model.standardBonePositions[8][2] - mz;                                  // 0x4A74E8..
        float bx = model.standardBonePositions[21][0] - mx, by = model.standardBonePositions[21][1] - my,
              bz = model.standardBonePositions[21][2] - mz;                                  // 0x4A7536..
        NormalizeCoordinates(ax, ay, az);                               // 0x4A756E
        NormalizeCoordinates(bx, by, bz);                               // 0x4A757B
        float cx = bz * ay - by * az;                              // 0x4A75A0
        float cy = bx * az - bz * ax;                              // 0x4A75C1
        float cz = by * ax - bx * ay;                              // 0x4A75DC
        const float dot = ay * by + bx * ax + az * bz;             // 0x4A75F8
        float axis[3] = {cx, cy, cz};
        RotationAxisFromDot((model.localTransforms + 60), axis, dot, ax, ay, az);             // 0x4A7622
    } else {
        model.localTransforms[63] = 1.0f;                                      // 0x4A765F
        model.localTransforms[60] = 0.0f;                                      // 0x4A7667
        model.localTransforms[61] = 0.0f;                                      // 0x4A766A
        model.localTransforms[62] = 0.0f;                                      // 0x4A7670
    }

    // ---- lower arm quat, model+96 (0x4A764E..0x4A7835) -------------------
    if (model.standardBonePositions[8][1] != kSentinel && model.standardBonePositions[9][1] != kSentinel &&
        model.standardBonePositions[2][1] != kSentinel) {
        quatRotationAxis(qA, kAxisZ, k35deg5311A4);                // 0x4A76CE
        if (model.standardBonePositions[21][1] == kSentinel) {                               // 0x4A76E4
            std::memcpy(qByVal, model.localTransforms + 0, sizeof(qByVal));
            BuildLookAtQuaternion(q150, qByVal, model.standardBonePositions[9][0], model.standardBonePositions[9][1], model.standardBonePositions[9][2],
                      model.standardBonePositions[8][0], model.standardBonePositions[8][1], model.standardBonePositions[8][2], 0);            // 0x4A77B7
        } else {
            d3->quatMultiply(qB, (model.localTransforms + 60), (model.localTransforms + 0));             // 0x4A76F0
            BuildLookAtQuaternion(q150, qB, model.standardBonePositions[9][0], model.standardBonePositions[9][1], model.standardBonePositions[9][2],
                      model.standardBonePositions[21][0], model.standardBonePositions[21][1], model.standardBonePositions[21][2], 0);            // 0x4A774F
        }
        d3->quatMultiply((model.localTransforms + 8), qA, q150);                      // 0x4A77E5
    } else if (flag != 0) {                                        // 0x4A77EE
        quatRotationAxis(qA, kAxisZ, -k35deg5311A4);               // 0x4A780A
        std::memcpy((model.localTransforms + 8), qA, 16);                             // 0x4A781B..
    }

    // ---- lower forearm quat, model+128 (0x4A7846..0x4A79CD) --------------
    if (model.standardBonePositions[9][1] != kSentinel && model.standardBonePositions[10][1] != kSentinel &&
        model.standardBonePositions[8][1] != kSentinel) {
        quatRotationAxis(qA, kAxisZ, k30deg53119C);                // 0x4A78A5
        d3->quatMultiply(t1, q150, (model.localTransforms + 60));                     // 0x4A78B8
        d3->quatMultiply(t2, t1, (model.localTransforms + 0));                        // 0x4A78CE
        BuildLookAtQuaternion(q155, t2, model.standardBonePositions[10][0], model.standardBonePositions[10][1], model.standardBonePositions[10][2],
                  model.standardBonePositions[9][0], model.standardBonePositions[9][1], model.standardBonePositions[9][2], 0);                // 0x4A793D
        d3->quatMultiply((model.localTransforms + 16), qA, q155);                     // 0x4A797D
        quatRotationAxis(qA, kAxisZ, -k30deg53119C);               // 0x4A7996
        d3->quatMultiply((model.localTransforms + 16), (model.localTransforms + 16), qA);                // 0x4A79A2
    } else if (flag != 0) {                                        // 0x4A79CD
        model.localTransforms[19] = 1.0f;                                      // 0x4A79CF
        model.localTransforms[16] = 0.0f;                                      // 0x4A79D7
        model.localTransforms[17] = 0.0f;                                      // 0x4A79DD
        model.localTransforms[18] = 0.0f;                                      // 0x4A79E3
    }

    // ---- lower wrist quat, model+112 (0x4A79C2..0x4A7C21) ----------------
    if (model.standardBonePositions[10][1] != kSentinel && model.standardBonePositions[11][1] != kSentinel &&
        model.standardBonePositions[9][1] != kSentinel) {
        if (model.standardBonePositions[11][0] == model.standardBonePositions[10][0] && model.standardBonePositions[11][1] == model.standardBonePositions[10][1] &&
            model.standardBonePositions[11][2] == model.standardBonePositions[10][2]) {                                // 0x4A7A64
            model.localTransforms[15] = 1.0f;                                  // 0x4A7A66
            model.localTransforms[12] = 0.0f;                                  // 0x4A7A6B
            model.localTransforms[13] = 0.0f;                                  // 0x4A7A6E
            model.localTransforms[14] = 0.0f;                                  // 0x4A7A71
        } else {
            quatRotationAxis(qA, kAxisZ, k30deg53119C);            // 0x4A7A93
            d3->quatMultiply(t1, q155, q150);                      // 0x4A7AAD
            d3->quatMultiply(t2, t1, (model.localTransforms + 60));                   // 0x4A7AC0
            d3->quatMultiply(qB, t2, (model.localTransforms + 0));                    // 0x4A7AD3
            BuildLookAtQuaternion(q159, qB, model.standardBonePositions[11][0], model.standardBonePositions[11][1], model.standardBonePositions[11][2],
                      model.standardBonePositions[10][0], model.standardBonePositions[10][1], model.standardBonePositions[10][2], 0);            // 0x4A7B42
            d3->quatMultiply((model.localTransforms + 12), qA, q159);                 // 0x4A7B7F
            quatRotationAxis(qA, kAxisZ, -k30deg53119C);           // 0x4A7B98
            d3->quatMultiply((model.localTransforms + 12), (model.localTransforms + 12), qA);            // 0x4A7BA4
        }
    } else if (flag != 0) {                                        // 0x4A7C21
        model.localTransforms[15] = 1.0f;                                      // 0x4A7C23
        model.localTransforms[12] = 0.0f;                                      // 0x4A7C28
        model.localTransforms[13] = 0.0f;                                      // 0x4A7C2B
        model.localTransforms[14] = 0.0f;                                      // 0x4A7C2E
    }

    // ---- lower body base quat, model+192 (0x4A7BC1..0x4A7E7B) -----------
    const float lmx = (model.standardBonePositions[16][0] + model.standardBonePositions[12][0]) * 0.5f;                // 0x4A7BC1
    const float lmy = (model.standardBonePositions[16][1] + model.standardBonePositions[12][1]) * 0.5f;                // 0x4A7BD1
    const float lmz = (model.standardBonePositions[16][2] + model.standardBonePositions[12][2]) * 0.5f;                // 0x4A7BE1
    if (model.standardBonePositions[1][1] != kSentinel && model.standardBonePositions[12][1] != kSentinel &&
        model.standardBonePositions[16][1] != kSentinel && lmy != kSentinel) {               // 0x4A7C16
        float ax = model.standardBonePositions[1][0] - lmx, ay = model.standardBonePositions[1][1] - lmy,
              az = model.standardBonePositions[1][2] - lmz;                                 // 0x4A7CAA..
        NormalizeCoordinates(ax, ay, az);                               // 0x4A7CE6
        float cx = az * 1.0f - ay * 0.0f;                          // 0x4A7D0B
        float cy = ax * 0.0f - az * 0.0f;                          // 0x4A7D2C
        float cz = ay * 0.0f - ax * 1.0f;                          // 0x4A7D47
        const float dot = 1.0f * ay + ax * 0.0f + 0.0f * az;       // 0x4A7D63
        float axis[3] = {cx, cy, cz};
        RotationAxisFromDot(qA, axis, dot, 0.0f, 1.0f, 0.0f);       // 0x4A7D8B
        BuildLookAtQuaternion(t1, qA, model.standardBonePositions[16][0], model.standardBonePositions[16][1], model.standardBonePositions[16][2],
                  model.standardBonePositions[12][0], model.standardBonePositions[12][1], model.standardBonePositions[12][2], 4);                // 0x4A7DFD
        std::memcpy((model.localTransforms + 32), t1, 16);                            // 0x4A7E04..
        d3->quatMultiply(t2, (model.localTransforms + 32), qA);                       // 0x4A7E2D
        std::memcpy((model.localTransforms + 32), t2, 16);                            // 0x4A7E48..
    } else if (flag != 0) {
        model.localTransforms[35] = 1.0f;                                      // 0x4A7E61
        model.localTransforms[32] = 0.0f;                                      // 0x4A7E69
        model.localTransforms[33] = 0.0f;                                      // 0x4A7E6F
        model.localTransforms[34] = 0.0f;                                      // 0x4A7E75
    }

    // ---- hip quat, model+256 (0x4A7E8A..0x4A7F7D) ------------------------
    if (model.standardBonePositions[12][1] != kSentinel && model.standardBonePositions[13][1] != kSentinel &&
        model.standardBonePositions[0][1] != kSentinel) {
        std::memcpy(qByVal, model.localTransforms + 32, sizeof(qByVal));
        BuildLookAtQuaternion(t1, qByVal, model.standardBonePositions[12][0], model.standardBonePositions[12][1], model.standardBonePositions[12][2],
                  model.standardBonePositions[13][0], model.standardBonePositions[13][1], model.standardBonePositions[13][2], 2);                // 0x4A7F47
        std::memcpy((model.localTransforms + 48), t1, 16);                            // 0x4A7F58..
    } else if (flag != 0) {                                        // 0x4A7F7D
        model.localTransforms[51] = 1.0f;                                      // 0x4A7F81
        model.localTransforms[48] = 0.0f;                                      // 0x4A7F89
        model.localTransforms[49] = 0.0f;                                      // 0x4A7F8F
        model.localTransforms[50] = 0.0f;                                      // 0x4A7F95
    }

    // ---- other hip quat, model+208 (0x4A7FAC..0x4A809F) ------------------
    if (model.standardBonePositions[16][1] != kSentinel && model.standardBonePositions[17][1] != kSentinel &&
        model.standardBonePositions[0][1] != kSentinel) {
        std::memcpy(qByVal, model.localTransforms + 32, sizeof(qByVal));
        BuildLookAtQuaternion(t1, qByVal, model.standardBonePositions[16][0], model.standardBonePositions[16][1], model.standardBonePositions[16][2],
                  model.standardBonePositions[17][0], model.standardBonePositions[17][1], model.standardBonePositions[17][2], 3);                // 0x4A8069
        std::memcpy((model.localTransforms + 36), t1, 16);                            // 0x4A807A..
    } else if (flag != 0) {                                        // 0x4A809F
        model.localTransforms[39] = 1.0f;                                      // 0x4A80A3
        model.localTransforms[36] = 0.0f;                                      // 0x4A80AB
        model.localTransforms[37] = 0.0f;                                      // 0x4A80B1
        model.localTransforms[38] = 0.0f;                                      // 0x4A80B7
    }

    // ---- knee quat #1, model+272 (0x4A80CE..0x4A81D1) --------------------
    if (model.standardBonePositions[13][1] != kSentinel && model.standardBonePositions[14][1] != kSentinel &&
        model.standardBonePositions[0][1] != kSentinel) {
        d3->quatMultiply(qB, (model.localTransforms + 48), (model.localTransforms + 32));                // 0x4A812C
        BuildLookAtQuaternion(t1, qB, model.standardBonePositions[13][0], model.standardBonePositions[13][1], model.standardBonePositions[13][2],
                  model.standardBonePositions[14][0], model.standardBonePositions[14][1], model.standardBonePositions[14][2], 1);                // 0x4A819B
        std::memcpy((model.localTransforms + 52), t1, 16);                            // 0x4A81AC..
    } else if (flag != 0) {                                        // 0x4A81D1
        model.localTransforms[55] = 1.0f;                                      // 0x4A81D5
        model.localTransforms[52] = 0.0f;                                      // 0x4A81DD
        model.localTransforms[53] = 0.0f;                                      // 0x4A81E3
        model.localTransforms[54] = 0.0f;                                      // 0x4A81E9
    }

    // ---- knee quat #2, model+224 (0x4A8200..0x4A831F) --------------------
    if (model.standardBonePositions[17][1] != kSentinel && model.standardBonePositions[18][1] != kSentinel &&
        model.standardBonePositions[0][1] != kSentinel) {
        d3->quatMultiply(qB, (model.localTransforms + 36), (model.localTransforms + 32));                // 0x4A825E
        BuildLookAtQuaternion(t1, qB, model.standardBonePositions[17][0], model.standardBonePositions[17][1], model.standardBonePositions[17][2],
                  model.standardBonePositions[18][0], model.standardBonePositions[18][1], model.standardBonePositions[18][2], 1);                // 0x4A82CD
        std::memcpy((model.localTransforms + 40), t1, 16);                            // 0x4A82DA..
    } else if (flag != 0) {                                        // 0x4A82FF
        model.localTransforms[43] = 1.0f;                                      // 0x4A8303
        model.localTransforms[40] = 0.0f;                                      // 0x4A8309
        model.localTransforms[41] = 0.0f;                                      // 0x4A830F
        model.localTransforms[42] = 0.0f;                                      // 0x4A8315
    }

    // ---- ankle/foot quat #1, model+288 (0x4A832E..0x4A864E) --------------
    if (model.standardBonePositions[15][1] != kSentinel && model.standardBonePositions[14][1] != kSentinel) {
        float ax = model.standardBonePositions[14][0] - model.standardBonePositions[13][0], ay = model.standardBonePositions[14][1] - model.standardBonePositions[13][1],
              az = model.standardBonePositions[14][2] - model.standardBonePositions[13][2];                            // 0x4A8365..
        NormalizeCoordinates(ax, ay, az);                               // 0x4A83A9
        float bx = model.standardBonePositions[15][0] - model.standardBonePositions[14][0], by = model.standardBonePositions[15][1] - model.standardBonePositions[14][1],
              bz = model.standardBonePositions[15][2] - model.standardBonePositions[14][2];                            // 0x4A83BA..
        NormalizeCoordinates(bx, by, bz);                               // 0x4A83FE
        const float dot = by * ay + bx * ax + bz * az;             // 0x4A841F
        bool straight = false;                                     // 0x4A849B
        if (model.standardBonePositions[15][2] - model.standardBonePositions[14][2] > 0.0f) {                          // 0x4A8458
            const float w = fabsf(model.standardBonePositions[15][0] - model.standardBonePositions[14][0]);            // 0x4A8470
            if (w < k75f && dot > 0.5f) straight = true;           // 0x4A8490
        }
        if (dot < kNeg0d3 || straight) {                           // 0x4A849D
            d3->quatMultiply(t1, (model.localTransforms + 52), (model.localTransforms + 48));            // 0x4A84B7
            d3->quatMultiply(t2, t1, (model.localTransforms + 32));                   // 0x4A84D0
            std::memcpy((model.localTransforms + 56), t2, 16);                        // 0x4A84E1..
        } else {
            float ex = 0.0f, ey = kNeg0d7, ez = -1.0f;             // 0x4A8508..
            NormalizeCoordinates(ex, ey, ez);                           // 0x4A8524
            float cx = bz * ey - by * ez;                          // 0x4A8549
            float cy = bx * ez - bz * ex;                          // 0x4A856A
            float cz = by * ex - bx * ey;                          // 0x4A8585
            const float dot2 = ey * by + bx * ex + ez * bz;        // 0x4A85A1
            float axis[3] = {cx, cy, cz};
            RotationAxisFromDot((model.localTransforms + 56), axis, dot2, ex, ey, ez);        // 0x4A85C9
        }
    } else {
        d3->quatMultiply(t1, (model.localTransforms + 52), (model.localTransforms + 48));                // 0x4A860E
        d3->quatMultiply(t2, t1, (model.localTransforms + 32));                       // 0x4A8627
        std::memcpy((model.localTransforms + 56), t2, 16);                            // 0x4A8638..
    }

    // ---- ankle/foot quat #2, model+240 (0x4A8669..0x4A899D) --------------
    if (model.standardBonePositions[19][1] != kSentinel && model.standardBonePositions[18][1] != kSentinel) {
        float ax = model.standardBonePositions[18][0] - model.standardBonePositions[17][0], ay = model.standardBonePositions[18][1] - model.standardBonePositions[17][1],
              az = model.standardBonePositions[18][2] - model.standardBonePositions[17][2];                            // 0x4A86A0..
        NormalizeCoordinates(ax, ay, az);                               // 0x4A86E4
        float bx = model.standardBonePositions[19][0] - model.standardBonePositions[18][0], by = model.standardBonePositions[19][1] - model.standardBonePositions[18][1],
              bz = model.standardBonePositions[19][2] - model.standardBonePositions[18][2];                            // 0x4A86F5..
        NormalizeCoordinates(bx, by, bz);                               // 0x4A8739
        const float dot = by * ay + bx * ax + bz * az;             // 0x4A875A
        bool straight = false;                                     // 0x4A87D6
        if (model.standardBonePositions[19][2] - model.standardBonePositions[18][2] > 0.0f) {                          // 0x4A8793
            const float w = fabsf(model.standardBonePositions[19][0] - model.standardBonePositions[18][0]);            // 0x4A87AB
            if (w < k75f && dot > 0.5f) straight = true;           // 0x4A87CB
        }
        if (dot < kNeg0d3 || straight) {                           // 0x4A87D8
            d3->quatMultiply(t1, (model.localTransforms + 40), (model.localTransforms + 36));            // 0x4A87F2
            d3->quatMultiply(t2, t1, (model.localTransforms + 32));                   // 0x4A880B
            std::memcpy((model.localTransforms + 44), t2, 16);                        // 0x4A881C..
        } else {
            float ex = 0.0f, ey = kNeg0d7, ez = -1.0f;             // 0x4A884B..
            NormalizeCoordinates(ex, ey, ez);                           // 0x4A8867
            float cx = bz * ey - by * ez;                          // 0x4A888C
            float cy = bx * ez - bz * ex;                          // 0x4A88AD
            float cz = by * ex - bx * ey;                          // 0x4A88C8
            const float dot2 = ey * by + bx * ex + ez * bz;        // 0x4A88E4
            float axis[3] = {cx, cy, cz};
            RotationAxisFromDot((model.localTransforms + 44), axis, dot2, ex, ey, ez);        // 0x4A890C
        }
    } else {
        d3->quatMultiply(t1, (model.localTransforms + 40), (model.localTransforms + 36));                // 0x4A895C
        d3->quatMultiply(t2, t1, (model.localTransforms + 32));                       // 0x4A8975
        std::memcpy((model.localTransforms + 44), t2, 16);                            // 0x4A8986..
    }
}

}  // namespace mikudancestudio
