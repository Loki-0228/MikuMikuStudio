#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mikumikustudio/pmm_key_map.hpp"
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include <atomic>
#include <thread>
#include <memory>
#include "mikumikustudio/background_save.hpp"
#include "pmm_save_versions.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include <filesystem>
#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/mesh_bake_cache.hpp"
#include <exception>
// ===========================================================================
// VA 0x0041B080 - SaveSceneFile  (original: sub_41B080, 0x3746 bytes)
// ===========================================================================
// Serialises the whole scene to the .pmm at app+0xA0900 (the caller fills
// that field from the save dialog first; the original ignores any argument
// and reads the path from the app object - it is a __thiscall member).
//
// Layout (write order is the original's, block VAs from the return-address
// landmarks left at every __write thunk call):
//   0x41B1B3  sprintf_s "Polygon Movie maker 0002" -> W(30)  [the 4 bytes
//             past the NUL are stack garbage in the original too]
//   0x41B1C4+ W4 render w/h (A08D4/A08D8), edit flag (A06C8 or A0D3C,
//             selected by the alt-dialog hwnd at A0D38), W4 fov (9E1E8),
//             W1 x6 option flags 0x2F8..0x2FD (bool), W1 slot index 0x910,
//             W1 (CB_GETCOUNT(item 0x1B4) - 1)
//   0x41B381  per existing model slot (0..99, byte = 0-based slot index):
//             name1 (len+bytes, model+0x2248), name2 (model+0x227A),
//             0x100-byte Shift-JIS path (WideToSjis of model+0x24BC),
//             W1 bone count byte (0x26D4), display-frame name table
//             (count 0x2D84, 0x25C-stride names at *0x26BC), morph name
//             table (0x2D80, 0x88 stride at *0x26C4), IK frame table
//             (0x2D88, W4 per 0x18-stride record at *0x26C0), rigid frame
//             table (0x4CCE8, W4 per 0x14-stride record at *0x4CCE4),
//             W1 0x2D7C, W1 bool 0x2D8D, W4 0x2D90, W4x5 0x2D9C..0x2DAC,
//             W1 bone-count byte again + per-bone bools (0x65-stride
//             records at *0x26D0, flag byte at +100), W4 0x31AC/0x31B0,
//             dense display-frame keys [0, count) (0x3C-stride records at
//             *0x26E0: W4 +0/+4/+8, W1 x4 (+0xC..+0x18), W4 x7
//             (+0x1C..+0x34), W1 bool +0x38/+0x39 - the dense loop counter
//             is 16-bit in the original and its record offset wraps with
//             it), then the sparse half: signed (count*0x3C <= off <
//             60*kBoneKeyCapacity bytes) scan counting frame!=0 records,
//             W4 count, then the
//             same records prefixed by W4(frame index); morph keys
//             (0x14-stride at *0x26E4, dense + sparse to 20000), rigid/IK
//             keys (0x1C-stride at *0x26E8: W4 +0/+4/+8 + W1 bool +0xC
//             base record; per record IK bool table at +0x10, float pairs
//             at +0x18, W1 bool +0x14; sparse scan 1..1000 re-emits the
//             per-record sub tables), then the current pose dump: per
//             display frame (0x25C records at *0x26BC) W4x7 +0x140..+0x158,
//             W1 bool +0x1ED, W1 bool *0x2D98[i], W1 bool *0x2D94[i]; per
//             morph W4 (+0x30 of the 0x88 records); per IK W1 bool +0x12;
//             per rigid W4 +4/+8/+0xC/+0x10 of the 0x14 records; finally
//             W1 bool 0x31BE, W4 0x31C0, W1 bool 0x37C0, W1 0x2D7D.
//   0x41C9AE  camera track (*0x374): W4x12 record 0 (+0..+0x24,+0x4C,+0x50),
//             W1 x24 (+0x28/+0x2E/+0x34/+0x3A + j, j=0..5), W1 bool +0x40,
//             W4 +0x44, W1 bool +0x48; sparse scan 1..9999 (0x54 stride)
//             each non-empty record prefixed by W4(frame index).
//   0x41CE85  camera misc W4s (0x334/0x338/0x33C/0x308/0x30C/0xA08DC/
//             0x310/0x314/0x318) + W1 bool 0x31C.
//   0x41CF51  light track (*0x378): W4 +0/+4/+8/+0x18/+0x1C/+0x20/+0xC/
//             +0x10/+0x14, W1 bool +0x24; sparse scan 1..9999 (0x28 stride).
//   0x41D207  light misc W4s (9E1A4/9E1A8/9E1AC/9E174/9E178/9E17C/9E170/
//             9DA48).
//   0x41D2AF  shadow list: W1 (u8)CB_GETCOUNT(item 0x1D7); per entry
//             CB_GETLBTEXT into the scratch buffer -> W(100).
//   0x41D310  accessory slots 0..254 (byte = slot index): W(acc+0x238,100),
//             0x100-byte SJIS path of acc+0x29C, W1 acc+0x49D, track
//             (*0x384[slot]) W4 +0/+4/+8, transparency byte
//             ((-0x1C - (int)(float(at +0x38) * 100.0)) * 2, +1 if the
//             byte at +0xC is set), W4 x9 (+0x10..+0x34 skipping +0x18),
//             W1 bool +0xD, W1 bool +0x18, sparse scan 1..9999 (0x3C
//             stride, transparency byte recomputed per record), then the
//             accessory-scale byte from acc+0x4A0 (+1 if acc+0x210),
//             W4 acc+0x230/+0x234/+0x220/+0x224/+0x228/+0x22C/+0x214/
//             +0x218/+0x21C, W1 bool acc+0x49C, W1 bool acc+0x49E.
//   0x41DB42  config: W4 0x980/0x97C/0x9E16C/0x914, W1 0x340 (raw),
//             W1 bool 0x341/0x342/0x9ED99, W4 atol(GetWindowText(item
//             0x199,8)) then W4 atol(item 0x19A,8), W1 bool 0xA06CC,
//             0x100-byte SJIS of 0xD0, if 0x9E400==0 swprintf_s(0x9E1EC,
//             0x100, L"%s") [see deviation note], W4 0x9E414/18/1C, 0x100
//             SJIS of 0x9E1EC, W4 0x91C, same quirk for 0x9E448 when
//             0x9E42C==0, W4 0x9E434/38/3C, 0x100 SJIS of 0x9E448,
//             W1 bool 0x9E428/0x31E/0x31D/0x918, W4 0xA08E0/0x9EB84/
//             0xA0B20/0xA0CF0, W1 bool 0x9ED9A, W1 0xA0CC4 (raw), W4
//             gravity 0x9EDC4/C8/B8/BC/C0, W1 bool 0xA0CD4.
//   0x41DF7C  selection/self-shadow track (*0x380): W4 +0/+4/+8, W1 bool
//             +0x20, W4 +0x1C/+0xC/+0x10/+0x14/+0x18, W1 bool +0x21;
//             sparse scan 1..9999 (0x24 stride).
//   0x41E25E  W1 bool 0xA0188, W4 0xA0D2C.
//   0x41E284  self-shadow track (*0x37C): W4 +0/+4/+8, W1 raw +0xC, W4
//             +0x10, W1 bool +0x14; sparse scan 1..9999 (0x18 stride).
//   0x41E46F  config2: W4 0xA0198/0xA019C/0xA01A0, W1 bool 0xA0194, W4
//             0xA0430..0xA0474 (18 dwords), W1 bool 0x9ED98/0xA0478/
//             0xA0197, W4 max(0, atol(GetWindowText(GetDlgItem(alt-or-
//             main, 0x22A), 10))), W1 constant 1.
//   0x41E70D  per existing model slot: W1 slot index, W4 model+0x4CCF0.
//   0x41E747  success tail: _close, swprintf_s title "MikuMikuDance [%s]"
//             with app+0xA0900, SetWindowTextW, MessageBeep(0x40),
//             app+0xA442C = 1.
// Error paths: path[0]=='\\' -> MessageBoxA JP/EN "cannot open save file"
// caption "save"; _wsopen_s errno -> sprintf_s JP/EN "%d" message with
// JP/EN caption.  0xA0B4C (EnglishUI) selects the branch; the JP texts are
// embedded here as raw Shift-JIS bytes from .rdata 0x52BCE4/0x52BD64/
// 0x52BD80.
//
// Deviations (docs/ARCHITECTURE.md section 8):
//   * swprintf_s(buf, 0x100, L"%s") is called with NO vararg in the
//     original (0x41DCC1/0x41DD7B - the CRT then reads a stack-garbage
//     pointer).  We pass buf itself so the call stays a no-op on the
//     already-valid string; the original's garbage cannot be reproduced.
//   * The 4 stack-garbage bytes after the header NUL and the garbage tails
//     of the 0x100-byte path writes are unspecified stack contents in the
//     original as well.  Our scratch buffer is zero-initialized once at
//     declaration (see below), so our tails are deterministic zeros on the
//     first conversion and previous-path residue afterwards - the
//     original's own garbage values are unreproducible and not attempted.
//   * Saving requires the model keyframe tracks (model+0x26BC/26C0/26C4/
//     26D0/26E0/26E4/26E8/4CCE4 and the count fields) to be allocated by
//     the model constructor; until that constructor is ported the counts
//     are 0 and the sparse scan would dereference a null track exactly
//     like the original would on such a model.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <io.h>
#include <utility>

#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/original_file_encoding.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"

// Shared PMM IO constants (window-title format; the loader record readers
// live there too).
#include "pmm_io_common.hpp"

namespace mikumikustudio {

// VA 0x00407910, defined in src/window/ui_dropfiles.cpp.
void WideToSjisPath(char* dst, const wchar_t* src, std::size_t size);

namespace {

// Preserve the legacy byte sequence, but do not report a partial save as success.
thread_local bool pmmWriteFailed = false;
thread_local std::vector<unsigned char>* pmmSnapshotSink = nullptr;
inline void W(int fd, const void* buf, unsigned int count) {
    if (pmmSnapshotSink) {
        const auto* bytes = static_cast<const unsigned char*>(buf);
        if (count) pmmSnapshotSink->insert(pmmSnapshotSink->end(), bytes, bytes + count);
    } else if (_write(fd, buf, count) != static_cast<int>(count)) pmmWriteFailed = true;
}

bool EncodePmmPath(char* out, std::size_t capacity, const wchar_t* value) {
#if MMD_ORIGINAL_PROFILE
    const bool encoded = original_file_encoding::Fixed(out, capacity, value);
    if (!encoded) pmmWriteFailed = true;
    return encoded;
#else
    return text_encoding::EncodePath(out, capacity, value);
#endif
}

#if MMD_ORIGINAL_PROFILE
template<class Key>
bool OriginalPmmTrack(const Key* keys, std::size_t denseCount,
                      std::size_t capacity, std::size_t originalSlots) {
    if (denseCount > originalSlots || denseCount > capacity) return false;
    for (std::size_t i = 0; i < capacity; ++i) {
        if (i >= denseCount && keys[i].frame == 0) continue;
        // PMM stores absolute slot indices and links, not merely a count.
        // The original loader uses them directly in its fixed-size pools.
        // An expanded but unused tail is harmless; one live high slot is not.
        if (i >= originalSlots || keys[i].previous >= originalSlots || keys[i].next >= originalSlots)
            return false;
    }
    return true;
}

bool OriginalPmmKeys(const MMDApp& app) {
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        const auto* raw = app.ModelSlot(slot);
        if (!raw) continue;
        const auto& model = *mdl::Mdl(raw);
        if (!OriginalPmmTrack(model.boneKeys, model.boneCount, mdl::BoneKeyCapacity(model), mdl::kBoneKeyCapacity) ||
            !OriginalPmmTrack(model.morphKeys, model.morphCount, mdl::MorphKeyCapacity(model), mdl::kMorphKeyCapacity))
            return false;
    }
    return true;
}

std::wstring AccessoryName(const mdl::AccessoryRecord& accessory) {
    const std::string bounded(accessory.name, strnlen_s(accessory.name, sizeof accessory.name));
    return text_encoding::Display(bounded.c_str());
}

bool OriginalPmmNames(MMDApp& app, bool& identityMismatch) {
    identityMismatch = false;
    std::string encoded;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        const auto* raw = app.ModelSlot(slot);
        if (!raw) continue;
        const auto& model = *mdl::Mdl(raw);
        for (int language = 0; language != 2; ++language)
            if (!original_file_encoding::Name(language ? model.nameEn : model.name, 20,
                    model.pmxTextBuffers[language], 255, encoded)) return false;
        for (unsigned i = 0; i < model.boneCount; ++i) {
            if (!original_file_encoding::Name(model.boneTable[i].name, 20,
                    model.boneTable[i].jpText, 20, encoded)) return false;
            if (encoded != std::string(model.boneTable[i].name, strnlen_s(model.boneTable[i].name, 20))) {
                identityMismatch = true; return false;
            }
        }
        for (unsigned i = 0; i < model.morphCount; ++i) {
            if (!original_file_encoding::Name(model.morphs[i].name, 20,
                    model.morphs[i].jpText, 20, encoded)) return false;
            if (encoded != std::string(model.morphs[i].name, strnlen_s(model.morphs[i].name, 20))) {
                identityMismatch = true; return false;
            }
        }
    }
    for (int slot = 0; slot < 255; ++slot)
        if (const auto* accessory = app.AccessorySlot(slot)) {
            const auto label = AccessoryName(*accessory);
            if (accessory->name[0] && label.empty()) return false;
            if (!original_file_encoding::Encode(label.c_str(), sizeof accessory->name - 1, encoded)) return false;
        }
    const HWND combo = GetDlgItem(app.MainWindow(), panel::kAccessoryCombo);
    const auto count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; ++i) {
        const auto length = SendMessageW(combo, CB_GETLBTEXTLEN, i, 0);
        if (length < 0 || length > 255) return false;
        wchar_t label[256]{};
        SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(label));
        if (!original_file_encoding::Encode(label, 99, encoded)) return false;
    }
    return true;
}

void WriteOriginalName(int fd, const char* raw, std::size_t capacity,
                       const wchar_t* unicode, std::size_t maximum) {
    std::string bytes;
    if (!original_file_encoding::Name(raw, capacity, unicode, maximum, bytes)) {
        pmmWriteFailed = true;
        return;
    }
    const auto length = static_cast<unsigned char>(bytes.size());
    W(fd, &length, 1);
    W(fd, bytes.data(), length);
}
#endif

void WritePmmBoneKey(int fd, const mdl::BoneKey& key) {
    W(fd, &key.frame, 4); W(fd, &key.previous, 4); W(fd, &key.next, 4);
    for (int lane = 0; lane < 4; ++lane) {
        W(fd, &key.interpolation[lane], 1);
        W(fd, &key.interpolation[lane + 4], 1);
        W(fd, &key.interpolation[lane + 8], 1);
        W(fd, &key.interpolation[lane + 12], 1);
    }
    W(fd, key.position, sizeof key.position);
    W(fd, key.rotation, sizeof key.rotation);
    const unsigned char allocated = key.allocated != 0;
    const unsigned char physicsDisabled = key.physicsDisabled != 0;
    W(fd, &allocated, 1); W(fd, &physicsDisabled, 1);
}

void WritePmmMorphKey(int fd, const mdl::MorphKey& key) {
    W(fd, &key.frame, 4); W(fd, &key.previous, 4); W(fd, &key.next, 4);
    W(fd, &key.value, 4);
    const unsigned char allocated = key.allocated != 0;
    W(fd, &allocated, 1);
}

void WritePmmDisplayKey(int fd, const mdl::DisplayKey& key,
                        int ikCount, int rigidCount) {
    W(fd, &key.frame, 4); W(fd, &key.previous, 4); W(fd, &key.next, 4);
    const unsigned char visible = key.visible != 0;
    W(fd, &visible, 1);
    const auto* flags = mdl::IkStates(key);
    for (int i = 0; i < ikCount; ++i) {
        const unsigned char enabled = flags[i] != 0;
        W(fd, &enabled, 1);
    }
    const auto* pairs = mdl::SelectorStates(key);
    for (int i = 0; i < rigidCount; ++i)
        W(fd, &pairs[i], sizeof(pairs[i]));
    const unsigned char allocated = key.allocated != 0;
    W(fd, &allocated, 1);
}

// strlen walked to the NUL as the original does, then truncated to the
// byte that the length slot holds (asm 0x41B3A8: mov [Buf], al / movzx).
inline unsigned char NameLen(const unsigned char* s) {
    const unsigned char* p = s;
    while (*p != 0) ++p;
    return static_cast<unsigned char>(p - s);
}

// Transparency/scale byte idiom (0x41D44E etc.): FPU control word | 0xC00
// (truncate), fistp of (float * 100.0 double), then
// byte = (char)(2 * (-0x1C - v)), +1 when the paired flag byte is set.
inline unsigned char ScaleQuirkByte(float scale01, unsigned char additive) {
    const int v = static_cast<int>(static_cast<double>(scale01) * 100.0);
    unsigned char b = static_cast<unsigned char>((-0x1C - v) * 2);
    if (additive != 0) b = static_cast<unsigned char>(b + 1);
    return b;
}

void WritePmmAccessoryKey(int fd, const mdl::AccessoryKey& key) {
    W(fd, &key.frame, 4);
    W(fd, &key.previous, 4);
    W(fd, &key.next, 4);
    const unsigned char visibleAndOpacity =
        ScaleQuirkByte(key.opacity, key.visible);
    W(fd, &visibleAndOpacity, 1);
    W(fd, &key.parentModel, 4);
    W(fd, &key.parentBone, 4);
    W(fd, key.position, sizeof key.position);
    W(fd, key.rotation, sizeof key.rotation);
    W(fd, &key.scale, 4);
    const unsigned char shadowEnabled = key.shadowEnabled != 0;
    const unsigned char selected = key.selected != 0;
    W(fd, &shadowEnabled, 1);
    W(fd, &selected, 1);
}

// ---- typed writers for the four global tracks -----------------------------
// Field order and widths are frozen against the original's raw-offset
// writes; each write below carries the old track+offset form it replaces,
// and the offsets are pinned by static_assert in global_key_layout.hpp.
// The byte streams match the loader readers in pmm_io_common.hpp exactly.

void WritePmmCameraKey(int fd, const mdl::CameraKey& key) {
    W(fd, &key.frame, 4);            // was W(fd, cam + 0x00, 4)
    W(fd, &key.previous, 4);         // was W(fd, cam + 0x04, 4)
    W(fd, &key.next, 4);             // was W(fd, cam + 0x08, 4)
    W(fd, &key.distance, 4);         // was W(fd, cam + 0x0C, 4)
    W(fd, &key.eye[0], 4);           // was W(fd, cam + 0x10, 4)
    W(fd, &key.eye[1], 4);           // was W(fd, cam + 0x14, 4)
    W(fd, &key.eye[2], 4);           // was W(fd, cam + 0x18, 4)
    W(fd, &key.target[0], 4);        // was W(fd, cam + 0x1C, 4)
    W(fd, &key.target[1], 4);        // was W(fd, cam + 0x20, 4)
    W(fd, &key.target[2], 4);        // was W(fd, cam + 0x24, 4)
    W(fd, &key.parentModel, 4);      // was W(fd, cam + 0x4C, 4)
    W(fd, &key.parentBone, 4);       // was W(fd, cam + 0x50, 4)
    for (int j = 0; j < 6; ++j) {    // was W(fd, cam + 0x28/0x2E/0x34/0x3A + j, 1)
        W(fd, &key.interpolation[0][j], 1);
        W(fd, &key.interpolation[1][j], 1);
        W(fd, &key.interpolation[2][j], 1);
        W(fd, &key.interpolation[3][j], 1);
    }
    {
        const unsigned char b = key.perspective != 0;    // was cam[0x40] != 0
        W(fd, &b, 1);
    }
    W(fd, &key.fov, 4);              // was W(fd, cam + 0x44, 4)
    {
        const unsigned char b = key.selected != 0;       // was cam[0x48] != 0
        W(fd, &b, 1);
    }
}

void WritePmmLightKey(int fd, const mdl::LightKey& key) {
    // File order: links, then color (+0x18..+0x20), then direction
    // (+0x0C..+0x14) - the memory order of the two float triplets differs.
    W(fd, &key.frame, 4);            // was W(fd, light + 0x00, 4)
    W(fd, &key.previous, 4);         // was W(fd, light + 0x04, 4)
    W(fd, &key.next, 4);             // was W(fd, light + 0x08, 4)
    W(fd, &key.color[0], 4);         // was W(fd, light + 0x18, 4)
    W(fd, &key.color[1], 4);         // was W(fd, light + 0x1C, 4)
    W(fd, &key.color[2], 4);         // was W(fd, light + 0x20, 4)
    W(fd, &key.direction[0], 4);     // was W(fd, light + 0x0C, 4)
    W(fd, &key.direction[1], 4);     // was W(fd, light + 0x10, 4)
    W(fd, &key.direction[2], 4);     // was W(fd, light + 0x14, 4)
    {
        const unsigned char b = key.selected != 0;       // was light[0x24] != 0
        W(fd, &b, 1);
    }
}

void WritePmmGravityKey(int fd, const mdl::GravityKey& key) {
    // Selection/gravity track (app+0x380, 36-byte records).  File order:
    // links, noiseEnabled (+0x20), then noise/acceleration/direction.
    W(fd, &key.frame, 4);            // was W(fd, sel + 0x00, 4)
    W(fd, &key.previous, 4);         // was W(fd, sel + 0x04, 4)
    W(fd, &key.next, 4);             // was W(fd, sel + 0x08, 4)
    {
        const unsigned char b = key.noiseEnabled != 0;   // was sel[0x20] != 0
        W(fd, &b, 1);
    }
    W(fd, &key.noise, 4);            // was W(fd, sel + 0x1C, 4)
    W(fd, &key.acceleration, 4);     // was W(fd, sel + 0x0C, 4)
    W(fd, &key.direction[0], 4);     // was W(fd, sel + 0x10, 4)
    W(fd, &key.direction[1], 4);     // was W(fd, sel + 0x14, 4)
    W(fd, &key.direction[2], 4);     // was W(fd, sel + 0x18, 4)
    {
        const unsigned char b = key.selected != 0;       // was sel[0x21] != 0
        W(fd, &b, 1);
    }
}

void WritePmmSelfShadowKey(int fd, const mdl::SelfShadowKey& key) {
    // Self-shadow track (app+0x37C, 24-byte records).  mode is written RAW
    // (the original copies the byte; it is not bool-ified).
    W(fd, &key.frame, 4);            // was W(fd, shadow + 0x00, 4)
    W(fd, &key.previous, 4);         // was W(fd, shadow + 0x04, 4)
    W(fd, &key.next, 4);             // was W(fd, shadow + 0x08, 4)
    W(fd, &key.mode, 1);             // was W(fd, shadow + 0x0C, 1) raw byte
    W(fd, &key.distance, 4);         // was W(fd, shadow + 0x10, 4)
    {
        const unsigned char b = key.selected != 0;       // was shadow[0x14] != 0
        W(fd, &b, 1);
    }
}

// Sparse-occupancy count over frames 1..9999 (record 0 is written densely
// above; the frame dword at record offset +0 is the occupancy test).  The
// original walks a dword pointer three records per iteration - camera
// p += 0x3F / light p += 0x1E / selection p += 0x1B / shadow p += 0x12
// dwords, i.e. 3 * sizeof(record) bytes each - reading the frame dword of
// records 3k+1/3k+2/3k+3.  Typed as a record walk of the same 9999 records.
template <typename TKey>
std::int32_t CountSparseTrackFrames(const TKey* track) {
    std::int32_t cnt = 0;
    for (std::size_t i = 1, capacity = mdl::KeyPoolCapacity(track); i < capacity; ++i)
        if (track[i].frame != 0) ++cnt;
    return cnt;
}

// Shift-JIS texts from the original .rdata (embedded byte-exact).
const char kJpSaveFailFmt[] =         // 0x52BCE4
    "\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xaa\x95\xdb\x91\xb6\x82\xc5"
    "\x82\xab\x82\xdc\x82\xb9\x82\xf1:%d";
const char kJpSaveFailCaption[] =     // 0x52BD64
    "\x83\x74\x83\x40\x83\x43\x83\x8b\x95\xdb\x91\xb6";
const char kJpCannotOpenText[] =      // 0x52BD80
    "\x82\xbb\x82\xcc\x83\x74\x83\x48\x83\x8b\x83\x5f\x82\xc9\x82\xcd"
    "\x83\x5a\x81\x5b\x83\x75\x82\xc5\x82\xab\x82\xdc\x82\xb9\x82\xf1";


// ---- SaveSceneFile segment functions -------------------------------------
// Split along the original VA map in the file header: each function below is
// the verbatim statement run of one numbered section (the section banner
// comments travel with the code).  The shared scratch buffer stays in
// SaveSceneFile's frame and is passed down, mirroring the original's single
// stack frame.

// 1. file header and global block (0x41B1B3..0x41B355); hdr is the caller's
// error-path sprintf buffer (stack -0x34 in the original), shared down.
void WritePmmFileHeader(int fd, MMDApp* s, HWND main, char* hdr, bool extended) {
    // ---- 1. file header and global block (0x41B1B3..0x41B355) -----------
    sprintf_s(hdr, 0x100, extended ? "Polygon Movie maker 0003" : "Polygon Movie maker 0002");
    // Bytes 25..29 after the 24-char magic + NUL are unspecified stack
    // residue in the original (its sprintf leaves the buffer's prior bytes
    // there); observed original saves carry zeros, so pin them to zero for
    // deterministic output (same deviation class as the text buffer below).
    hdr[25] = hdr[26] = hdr[27] = hdr[28] = hdr[29] = 0;
    W(fd, hdr, 0x1E);                                          // 0x41B1C4

    W(fd, &s->state.renderW, 4);      // 0x41B1D7
    W(fd, &s->state.renderH, 4);      // 0x41B1EA
    {
        const std::int32_t v = reinterpret_cast<const std::int32_t&>(
            s->state.floatingWindow);
        if (v == 0)
            W(fd, &s->state.sidebarWidth, 4);
        else
            W(fd, &s->state.separateWindowSidebarWidth, 4);
    }
    W(fd, &s->state.cameraFov, 4);        // 0x41B22E

    for (int f = 0; f < 7; ++f) {                              // 0x41B22E..0x41B303
        const unsigned char b =
            s->state.optflag[f] != 0;               // 0x2F8..0x2FE
        W(fd, &b, 1);
    }
    W(fd, &s->SelectedModelSlot(), 1);                         // 0x41B31A

    {                                                          // 0x41B33F
        const unsigned char b = static_cast<unsigned char>(
            SendMessageA(GetDlgItem(main, panel::kMainComboModel), CB_GETCOUNT, 0, 0) - 1);
        W(fd, &b, 1);
    }
}


// 2. per-model block (0x41B355..0x41C981)
void WritePmmModelBlocks(int fd, unsigned char** const slots,
                         char* text) {
    // ---- 2. per-model block (0x41B355..0x41C981) ------------------------
    // Not a file-format cap: the x64 save twin sub_7FF7CB4950A0 writes the
    // slot id byte for every occupied slot of the full array (inc dl /
    // cmp dl, 0FFh / jb at 0x7FF7CB496C11..0x7FF7CB496C1A), so the on-disk
    // bound is the slot capacity (0..254 on x64).
    for (unsigned char slot = 0; slot < kModelSlotCount; ++slot) {  // loc_41B360
        if (slots[slot] == 0) continue;
        W(fd, &slot, 1);                                       // 0x41B381
        unsigned char* const model = slots[slot];

        for (int language = 0; language < 2; ++language) {
#if MMD_ORIGINAL_PROFILE
            const auto& record = *mdl::Mdl(model);
            WriteOriginalName(fd, language ? record.nameEn : record.name, 20,
                              record.pmxTextBuffers[language], 255);
#else
            const char* name = language ? mdl::Mdl(model)->nameEn : mdl::Mdl(model)->name;
            char exact[256]{};
            const auto wide = text_encoding::ModelName(*mdl::Mdl(model), language != 0);
            if(text_encoding::EncodeDisplay(exact,sizeof exact,wide.c_str())) name=exact;
            const auto len = static_cast<unsigned char>(std::strlen(name));
            W(fd, &len, 1);
            W(fd, name, len);
#endif
        }

        EncodePmmPath(text, 0x100, mdl::Mdl(model)->path);
        W(fd, text, 0x100);                                    // 0x41B471

        const mdl::ModelRecord* const record = mdl::Mdl(model);
        const std::int32_t dispCount =
            static_cast<std::int32_t>(record->boneCount);
        const std::int32_t morphCount =
            static_cast<std::int32_t>(record->morphCount);
        const std::int32_t ikCount =
            static_cast<std::int32_t>(record->ikChainCount);
        const std::int32_t rigidCount =
            static_cast<std::int32_t>(record->boneOrderCount);
        // Runtime pool capacity, resolved once.  The sparse bone-key half below
        // is an index scan over the whole pool; a bound fixed at the original
        // 600000 would silently drop every key the pool holds beyond it.
        const std::int32_t boneKeyCapacity =
            static_cast<std::int32_t>(mdl::BoneKeyCapacity(*record));
        mdl::BoneRecord* const bones = mdl::Bones(model);
        mdl::MorphRecord* const morphs = mdl::Morphs(model);
        mdl::IkChain* const ikChains = mdl::IkChains(model);
        mdl::BoneOrderEntry* const rigidFrames =
            mdl::BoneOrder(model);
        mdl::BoneKey* const dispKeys = mdl::BoneKeys(model);
        mdl::MorphKey* const morphKeys = mdl::MorphKeys(model);
        mdl::DisplayKey* const physKeys = mdl::DisplayKeys(model);
        const PmmKeyMap<mdl::BoneKey> boneMap(dispKeys, dispCount, boneKeyCapacity, mdl::kBoneKeyCapacity);
        const PmmKeyMap<mdl::MorphKey> morphMap(morphKeys, morphCount, mdl::MorphKeyCapacity(morphKeys), mdl::kMorphKeyCapacity);
        const PmmKeyMap<mdl::DisplayKey> displayMap(physKeys, 1, mdl::KeyPoolCapacity(physKeys), mdl::kDisplayKeyCapacity);

        W(fd, &record->groupCount, 1);                         // 0x41B48E
        W(fd, &record->boneCount, sizeof(record->boneCount));  // 0x41B4AC
        if (dispCount != 0 && dispCount > 0) {
            for (std::int32_t i = 0; i < dispCount; ++i) {
#if MMD_ORIGINAL_PROFILE
                WriteOriginalName(fd, bones[i].name, 20, bones[i].jpText, 20);
#else
                const unsigned char len = NameLen(
                    reinterpret_cast<const unsigned char*>(bones[i].name));
                W(fd, &len, 1);                                // 0x41B4FF
                W(fd, bones[i].name, len);                     // 0x41B524
#endif
            }
        }
        W(fd, &record->morphCount, sizeof(record->morphCount)); // 0x41B562
        if (morphCount != 0 && morphCount > 0) {
            for (std::int32_t i = 0; i < morphCount; ++i) {
#if MMD_ORIGINAL_PROFILE
                WriteOriginalName(fd, morphs[i].name, 20, morphs[i].jpText, 20);
#else
                const unsigned char len = NameLen(
                    reinterpret_cast<const unsigned char*>(morphs[i].name));
                W(fd, &len, 1);                                // 0x41B5AF
                W(fd, morphs[i].name, len);                    // 0x41B5D4
#endif
            }
        }
        W(fd, &record->ikChainCount, sizeof(record->ikChainCount)); // 0x41B612
        if (ikCount != 0 && ikCount > 0) {
            for (std::int32_t i = 0; i < ikCount; ++i)
                W(fd, &ikChains[i].boneIndex, sizeof(ikChains[i].boneIndex));
        }
        W(fd, &record->boneOrderCount,
          sizeof(record->boneOrderCount));                     // 0x41B680
        if (rigidCount != 0 && rigidCount > 0) {
            for (std::int32_t i = 0; i < rigidCount; ++i)
                W(fd, &rigidFrames[i].boneIndex,
                  sizeof(rigidFrames[i].boneIndex));           // 0x41B6B5
        }
        W(fd, &record->comboSelIndex, 1);                      // 0x41B6EF
        {
            const unsigned char b = record->loadComplete != 0;
            W(fd, &b, 1);                                      // 0x41B719
        }
        W(fd, &record->selectedBone, sizeof(record->selectedBone)); // 0x41B737
        for (const std::int32_t selectedMorph :
             mdl::Mdl(model)->selectedMorphs)
            W(fd, &selectedMorph, sizeof(selectedMorph));      // 0x41B75B

        const unsigned char boneCount = record->groupCount;
        W(fd, &record->groupCount, 1);                         // 0x41B790
        if (boneCount != 0) {
            const mdl::DisplayGroup* const groups =
                mdl::DisplayGroups(model);
            for (unsigned char i = 0; i < boneCount; ++i) {    // 0x41B7D5
                const unsigned char b =
                    groups[i].flags != 0;
                W(fd, &b, 1);
            }
        }
        W(fd, &record->boneListPos, sizeof(record->boneListPos)); // 0x41B800
        W(fd, &record->maxFrame, sizeof(record->maxFrame));    // 0x41B81F

        // dense display-frame key records (0x41B860..0x41BAA4); the
        // original walks both the counter and the record offset through
        // a 16-bit register (movzx eax, bx at 0x41BABA), so the pair
        // wraps together past 65535.
        if (dispCount != 0 && dispCount > 0) {
            // The 16-bit counter was compared against the 32-bit dispCount:
            // beyond 65535 it wraps back to 0, so the condition stays true and the
            // save never returns, rewriting the same records from the start
            // of the pool. The original dense loop also runs on a 16-bit register
            // (see the movzx eax, bx note above), but that original
            // site could not be found in the x64 reference image, so this is
            // deliberate hardening: the counter is widened to stop at
            // dispCount, where the sparse half below resumes. The
            // bytes written and their order are unchanged while boneCount is
            // at most 65535.
            // Defect B3-24.
            for (std::int32_t i = 0; i < dispCount; ++i) {
                WritePmmBoneKey(fd, boneMap.Remap(dispKeys[i]));
            }
        }
        // sparse half: signed scan count*0x3C <= off < 60*pool capacity
        {                                                      // 0x41BACC
            std::int32_t cnt = 0;
            if (dispCount < boneKeyCapacity) {
                for (std::int32_t i = dispCount;
                     i < boneKeyCapacity; ++i)
                    if (dispKeys[i].frame != 0)
                        ++cnt;
            }
            W(fd, &cnt, 4);                                    // 0x41BB2D
            if (dispCount < boneKeyCapacity) {                          // 0x41BB44
                for (std::int32_t i = dispCount;
                     i < boneKeyCapacity; ++i) {
                    if (dispKeys[i].frame == 0)
                        continue;
                    const auto savedIndex = boneMap.Index(i); W(fd, &savedIndex, 4);                              // 0x41BB92
                    WritePmmBoneKey(fd, boneMap.Remap(dispKeys[i]));
                }
            }
        }

        // morph keys: dense then sparse (0x41BEE8..0x41C13A)
        if (morphCount != 0 && morphCount > 0) {
            for (std::int32_t i = 0; i < morphCount; ++i)
                WritePmmMorphKey(fd, morphMap.Remap(morphKeys[i]));
        }
        {                                                      // 0x41BFFD
            std::int32_t cnt = 0;
            if (morphCount < mdl::MorphKeyCapacity(morphKeys)) {
                const auto morphCapacity1 = mdl::MorphKeyCapacity(morphKeys);
                for (std::int32_t i = morphCount; i < morphCapacity1; ++i)
                    if (morphKeys[i].frame != 0)
                        ++cnt;
            }
            W(fd, &cnt, 4);
            if (morphCount < mdl::MorphKeyCapacity(morphKeys)) {
                const auto morphCapacity2 = mdl::MorphKeyCapacity(morphKeys);
                for (std::int32_t i = morphCount; i < morphCapacity2; ++i) {
                    if (morphKeys[i].frame == 0)
                        continue;
                    const auto savedIndex = morphMap.Index(i); W(fd, &savedIndex, 4);                              // 0x41C05E
                    WritePmmMorphKey(fd, morphMap.Remap(morphKeys[i]));
                }
            }
        }

        // rigid/IK base record (0x41C173..)
        WritePmmDisplayKey(fd, displayMap.Remap(physKeys[0]), ikCount, rigidCount);
        {  // sparse rigid/IK scan, 1000 records of 0x1C (0x41C353..)
            std::int32_t cnt = 0;
            for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(physKeys)); i < poolCapacity; ++i)
                if (physKeys[i].frame != 0) ++cnt;
            W(fd, &cnt, 4);
            for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(physKeys)); i < poolCapacity; ++i) {          // 0x41C394
                if (physKeys[i].frame == 0)
                    continue;
                const auto savedIndex = displayMap.Index(i); W(fd, &savedIndex, 4);
                WritePmmDisplayKey(fd, displayMap.Remap(physKeys[i]), ikCount, rigidCount);
            }
        }

        // current pose dump (0x41C5EB..0x41C8AD)
        if (dispCount != 0 && dispCount > 0) {
            unsigned char* const physicsState = record->bonePhysicsState;
            unsigned char* const selection = record->boneSelection;
            for (std::int32_t i = 0; i < dispCount; ++i) {
                W(fd, &bones[i].trans[0], 4);                   // 0x41C5EB
                W(fd, &bones[i].trans[1], 4);                   // 0x41C611
                W(fd, &bones[i].trans[2], 4);                   // 0x41C637
                W(fd, &bones[i].rotQuat[0], 4);                 // 0x41C65D
                W(fd, &bones[i].rotQuat[1], 4);                 // 0x41C683
                W(fd, &bones[i].rotQuat[2], 4);                 // 0x41C6A9
                W(fd, &bones[i].rotQuat[3], 4);                 // 0x41C6D2
                {
                    const unsigned char b = bones[i].physicsDisabled != 0;
                    W(fd, &b, 1);                              // 0x41C703
                }
                {
                    const unsigned char b = physicsState[i] != 0; // 0x41C730
                    W(fd, &b, 1);
                }
                {
                    const unsigned char b = selection[i] != 0; // 0x41C75D
                    W(fd, &b, 1);
                }
            }
        }
        if (morphCount != 0 && morphCount > 0) {
            for (std::int32_t i = 0; i < morphCount; ++i)
                W(fd, &morphs[i].value, sizeof(morphs[i].value));
        }
        if (ikCount != 0 && ikCount > 0) {
            for (std::int32_t i = 0; i < ikCount; ++i) {
                const unsigned char b = ikChains[i].enabled != 0;
                W(fd, &b, 1);                                  // 0x41C812
            }
        }
        if (rigidCount != 0 && rigidCount > 0) {
            for (std::int32_t i = 0; i < rigidCount; ++i) {
                W(fd, &rigidFrames[i].windowStart, 4);         // 0x41C867
                W(fd, &rigidFrames[i].windowEnd, 4);           // 0x41C88A
                W(fd, &rigidFrames[i].linkedModel, 4);         // 0x41C8AD
                W(fd, &rigidFrames[i].linkedBone, 4);          // 0x41C8D0
            }
        }
        {
            const unsigned char b =                            // 0x41C91A
                record->postLoadFlag2 != 0;
            W(fd, &b, 1);
        }
        W(fd, &record->edgeScale, sizeof(record->edgeScale));  // 0x41C939
        {
            const unsigned char b =                            // 0x41C963
                record->toonFlag != 0;
            W(fd, &b, 1);
        }
        const unsigned char displayOrder = mdl::Mdl(model)->comboSelIndex2;
        W(fd, &displayOrder, sizeof(displayOrder));             // 0x41C981
    }
}


// 3. camera track + camera misc (0x41C9AE..0x41CF3E)
void WritePmmCameraSection(int fd, MMDApp* s) {
    // ---- 3. camera track (0x41C9AE..0x41CE85) ---------------------------
    const mdl::CameraKey* const cam = s->CameraKeys();  // app+0x374
    const PmmKeyMap<mdl::CameraKey> camMap(cam, 1, mdl::KeyPoolCapacity(cam), 10000);
    WritePmmCameraKey(fd, camMap.Remap(cam[0]));                              // 0x41C9AE
    {  // sparse scan over camera keys 1..9999 (sizeof(mdl::CameraKey) stride)
        const std::int32_t cnt = CountSparseTrackFrames(cam);   // 0x41CBB8..
        W(fd, &cnt, 4);
        for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(cam)); i < poolCapacity; ++i) {             // 0x41CBE6
            if (cam[i].frame == 0)
                continue;
            const auto savedIndex = camMap.Index(i); W(fd, &savedIndex, 4);
            WritePmmCameraKey(fd, camMap.Remap(cam[i]));                      // 0x41CC00..
        }
    }

    // camera misc (0x41CE85..0x41CF3E)
    W(fd, &s->state.cameraPosX, 4);         // 0x334
    W(fd, &s->state.cameraPosY, 4);         // 0x338
    W(fd, &s->state.cameraPosZ, 4);         // 0x33C
    W(fd, &s->state.viewOffsetX, 4);         // 0x308
    W(fd, &s->state.viewOffsetY, 4);         // 0x30C
    W(fd, &s->state.cameraDistance, 4);     // 0xA08DC
    W(fd, &s->state.cameraPitch, 4);         // 0x310
    W(fd, &s->state.cameraYaw, 4);         // 0x314
    W(fd, &s->state.cameraRoll, 4);         // 0x318
    {
        const unsigned char b =                                // 0x31C
            s->state.cameraPerspective != 0;
        W(fd, &b, 1);
    }
}


// 4. light track + light misc (0x41CF51..0x41D28F)
void WritePmmLightSection(int fd, MMDApp* s) {
    // ---- 4. light track (0x41CF51..0x41D207) ----------------------------
    const mdl::LightKey* const light = s->LightKeys();  // app+0x378
    const PmmKeyMap<mdl::LightKey> lightMap(light, 1, mdl::KeyPoolCapacity(light), 10000);
    WritePmmLightKey(fd, lightMap.Remap(light[0]));                             // 0x41CF51
    {  // sparse scan over light keys 1..9999 (sizeof(mdl::LightKey) stride)
        const std::int32_t cnt = CountSparseTrackFrames(light); // 0x41D072..
        W(fd, &cnt, 4);
        for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(light)); i < poolCapacity; ++i) {             // 0x41D0A3
            if (light[i].frame == 0)
                continue;
            const auto savedIndex = lightMap.Index(i); W(fd, &savedIndex, 4);
            WritePmmLightKey(fd, lightMap.Remap(light[i]));                     // 0x41D0C0..
        }
    }

    // light misc (0x41D207..0x41D28F)
    W(fd, s->LightColor() + 0, sizeof(float));
    W(fd, s->LightColor() + 1, sizeof(float));
    W(fd, s->LightColor() + 2, sizeof(float));
    W(fd, s->LightDirection() + 0, sizeof(float));
    W(fd, s->LightDirection() + 1, sizeof(float));
    W(fd, s->LightDirection() + 2, sizeof(float));
    W(fd, &s->SelectedAccessorySlot(), 1);                      // 0x41D26D 1 byte
    W(fd, &s->DisplayObjectListScrollPosition(), 4);
}


// 5. accessory-shadow list (0x41D2AF..0x41D310)
void WritePmmAccessoryShadowList(int fd, HWND main, char* text) {
    // ---- 5. accessory-shadow list (0x41D2AF..0x41D310) ------------------
    {
        const unsigned char cnt = static_cast<unsigned char>(
            SendMessageA(GetDlgItem(main, panel::kAccessoryCombo), CB_GETCOUNT, 0, 0));
        W(fd, &cnt, 1);                                        // 0x41D2CD
        for (unsigned int i = 0; i < cnt; ++i) {               // 0x41D2FC
            wchar_t label[256]{};
            SendMessageW(GetDlgItem(main, panel::kAccessoryCombo), CB_GETLBTEXT, i,
                         reinterpret_cast<LPARAM>(label));
#if MMD_ORIGINAL_PROFILE
            if (!original_file_encoding::Fixed(text, 100, label)) pmmWriteFailed = true;
#else
            text_encoding::EncodeDisplay(text, 100, label);
#endif
            W(fd, text, 100);                                  // 0x41D310
        }
    }
}


// 6. accessory block (0x41D310..0x41DB42)
void WritePmmAccessoryBlocks(
    int fd, mdl::AccessoryRecord** const accessories,
    mdl::AccessoryKey** const accTracks, char* text) {
    // ---- 6. accessory block (0x41D310..0x41DB42) ------------------------
    for (unsigned char slot = 0; slot != 0xFF; ++slot) {       // 0x41D351
        if (accessories[slot] == 0) continue;
        W(fd, &slot, 1);
        const mdl::AccessoryRecord& accessory =
            *mdl::Accessory(accessories[slot]);
        const auto* const track = reinterpret_cast<const mdl::AccessoryKey*>(
            accTracks[slot]);

#if MMD_ORIGINAL_PROFILE
        const auto label = AccessoryName(accessory);
        if (!original_file_encoding::Fixed(text, sizeof accessory.name, label.c_str())) pmmWriteFailed = true;
        W(fd, text, sizeof accessory.name);
#else
        W(fd, accessory.name, sizeof accessory.name);          // 0x41D370
#endif
        EncodePmmPath(text, 0x100, accessory.sourcePath);
        W(fd, text, 0x100);                                    // 0x41D3AF
        W(fd, &accessory.order, 1);                             // 0x41D3CD

            const PmmKeyMap<mdl::AccessoryKey> trackMap(track, 1, mdl::KeyPoolCapacity(track), 10000);
    WritePmmAccessoryKey(fd, trackMap.Remap(track[0]));
        {  // sparse scan over accessory keys 1..9999 (0x3C stride)
            std::int32_t cnt = 0;
            for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(track)); i < poolCapacity; ++i)
                if (track[i].frame != 0) ++cnt;
            W(fd, &cnt, 4);
            for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(track)); i < poolCapacity; ++i) {         // 0x41D66E
                if (track[i].frame == 0)
                    continue;
                const auto savedIndex = trackMap.Index(i); W(fd, &savedIndex, 4);
                WritePmmAccessoryKey(fd, trackMap.Remap(track[i]));
            }
        }
        {
            const unsigned char b = ScaleQuirkByte(            // 0x41D9AC
                accessory.opacity, accessory.visible);
            W(fd, &b, 1);
        }
        W(fd, &accessory.parentModel, 4);                       // 0x41D9CB
        W(fd, &accessory.parentBone, 4);                        // 0x41D9EA
        W(fd, &accessory.rotation[0], 4);                       // 0x41DA09
        W(fd, &accessory.rotation[1], 4);                       // 0x41DA28
        W(fd, &accessory.rotation[2], 4);                       // 0x41DA47
        W(fd, &accessory.scale, 4);                             // 0x41DA69
        W(fd, &accessory.position[0], 4);                       // 0x41DA88
        W(fd, &accessory.position[1], 4);                       // 0x41DAA7
        W(fd, &accessory.position[2], 4);                       // 0x41DAC6
        {
            const unsigned char b = accessory.shadowEnabled != 0;
            W(fd, &b, 1);
        }
        {
            const unsigned char b = accessory.additiveBlend != 0;
            W(fd, &b, 1);
        }
    }
}


// 7. config block (0x41DB42..0x41DF7C)
void WritePmmConfigBlock(int fd, MMDApp* s, HWND main, char* text) {
    // ---- 7. config block (0x41DB42..0x41DF7C) ---------------------------
    const auto standardFrame=(std::max)(0,s->CurrentFrame());
    const auto standardScroll=(std::max)(0,s->state.timelineStartFrame);
    W(fd, &standardFrame, 4);
    W(fd, &standardScroll, 4);
    W(fd, &s->state.lastRegisteredFrame, 4);
    const std::int32_t savedEditMode = static_cast<std::int32_t>(s->EditMode());
    W(fd, &savedEditMode, 4);
    W(fd, &s->state.cameraReferenceMode, 1);           // raw byte
    {
        const unsigned char b = s->state.playbackLoopEnabled != 0;
        W(fd, &b, 1);
    }
    {
        const unsigned char b = s->state.playbackReturnsToStartFrame != 0;
        W(fd, &b, 1);
    }
    {
        const unsigned char b = s->state.playbackStartsAtCurrentFrame != 0;
        W(fd, &b, 1);
    }
    {  // frame edit readbacks 0x199 then 0x19A (asm 0x41DBEA..0x41DC5F)
        text[0] = 0;
        GetWindowTextA(GetDlgItem(main, panel::kPlayStartFrameEdit), text, 256);
        std::int32_t playStartFrame = 0;
        if (!frame_limits::ParsePlaybackBound(text, playStartFrame)) throw std::runtime_error("Invalid playback start frame");
        text[0] = 0;
        GetWindowTextA(GetDlgItem(main, panel::kPlayStopFrameEdit), text, 256);
        std::int32_t playStopFrame = 0;
        if (!frame_limits::ParsePlaybackBound(text, playStopFrame)) throw std::runtime_error("Invalid playback stop frame");
        std::int32_t standardStart=0,standardStop=0;
        frame_limits::ResolveRange(playStartFrame,playStopFrame,s->LastRegisteredFrame(),standardStart,standardStop,true);
        W(fd, &standardStart, 4);
        W(fd, &standardStop, 4);
    }
    {
        const unsigned char b = s->state.waveEnabled != 0;
        W(fd, &b, 1);
    }
    EncodePmmPath(text, 0x100, s->WavePath());
    W(fd, text, 0x100);                                        // 0x41DCB5
    W(fd, &s->AviOffsetX(), 4);
    W(fd, &s->AviOffsetY(), 4);
    W(fd, &s->AviScale(), 4);
    EncodePmmPath(text, 0x100, s->AviBackgroundPath());
    W(fd, text, 0x100);                                        // 0x41DD46
    W(fd, &s->AviBackgroundEnabled(), 4);
    W(fd, &s->PictureOffsetX(), 4);
    W(fd, &s->PictureOffsetY(), 4);
    W(fd, &s->PictureScale(), 4);
    EncodePmmPath(text, 0x100, s->PictureBackgroundPath());
    W(fd, text, 0x100);                                        // 0x41DDEA
    {
        const unsigned char b = s->PictureBackgroundEnabled() != 0;
        W(fd, &b, 1);
    }
    {
        const unsigned char b = s->state.fpsOverlayEnabled != 0;
        W(fd, &b, 1);
    }
    {
        const unsigned char b = s->state.groundGridEnabled != 0;
        W(fd, &b, 1);
    }
    {
        const unsigned char b = s->state.groundShadowEnabled != 0;
        W(fd, &b, 1);
    }
    W(fd, &s->state.fpsLimit, 4);     // 0xA08E0
    W(fd, &reinterpret_cast<ScreenCaptureMode&>(s->state.captureMode), 4);
    W(fd, &s->state.accessoryRenderSplitOrder, 4);
    W(fd, &s->ProjectedShadowAmbientIntensity(), 4);
    {
        const unsigned char b = s->state.projectedShadowBlendEnabled != 0;
        W(fd, &b, 1);
    }
    W(fd, reinterpret_cast<const unsigned char*>(
              &s->PlaybackPhysicsMode()), 1);                  // raw byte
    W(fd, &s->state.gravityMagnitude, 4);      // 0x9EDC4
    W(fd, &s->state.gravityNoise, 4);
    W(fd, &s->state.gravityX, 4);        // 0x9EDB8
    W(fd, &s->state.gravityY, 4);        // 0x9EDBC
    W(fd, &s->state.gravityZ, 4);        // 0x9EDC0
    {
        const unsigned char b = s->state.gravityNoiseEnabled != 0;
        W(fd, &b, 1);
    }
}


// 8. selection track + shadow-mode byte (0x41DF7C..0x41E284)
void WritePmmSelectionSection(int fd, MMDApp* s) {
    // ---- 8. selection/self-shadow track (0x41DF7C..0x41E25E) ------------
    const mdl::GravityKey* const sel = s->GravityKeys();  // app+0x380
    const PmmKeyMap<mdl::GravityKey> selMap(sel, 1, mdl::KeyPoolCapacity(sel), 10000);
    WritePmmGravityKey(fd, selMap.Remap(sel[0]));                             // 0x41DF7C
    {  // sparse scan over selection keys 1..9999
       // (sizeof(mdl::GravityKey) stride)
        const std::int32_t cnt = CountSparseTrackFrames(sel);   // 0x41E0AC..
        W(fd, &cnt, 4);
        for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(sel)); i < poolCapacity; ++i) {             // 0x41E0E4
            if (sel[i].frame == 0)
                continue;
            const auto savedIndex = selMap.Index(i); W(fd, &savedIndex, 4);
            WritePmmGravityKey(fd, selMap.Remap(sel[i]));                     // 0x41E101..
        }
    }

    {
        const unsigned char b = s->state.selfShadowEnabled != 0;
        W(fd, &b, 1);                                          // 0x41E25E
    }
    W(fd, &s->state.physicsInterval, 4);        // 0xA0D2C
}


// 9. self-shadow track + 10. config2 (0x41E284..0x41E6E5); kept as one
// function - the config2 run continues the same write sequence.
void WritePmmShadowAndConfig2(int fd, MMDApp* s, HWND main, char* text) {
    // ---- 9. self-shadow track (0x41E284..0x41E46F) ----------------------
    const mdl::SelfShadowKey* const shadow = s->ShadowKeys();  // app+0x37C
    const PmmKeyMap<mdl::SelfShadowKey> shadowMap(shadow, 1, mdl::KeyPoolCapacity(shadow), 10000);
    WritePmmSelfShadowKey(fd, shadowMap.Remap(shadow[0]));                       // 0x41E284
    {  // sparse scan over self-shadow keys 1..9999
       // (sizeof(mdl::SelfShadowKey) stride)
        const std::int32_t cnt = CountSparseTrackFrames(shadow);// 0x41E34D..
        W(fd, &cnt, 4);
        for (std::int32_t i = 1, poolCapacity = static_cast<std::int32_t>(mdl::KeyPoolCapacity(shadow)); i < poolCapacity; ++i) {             // 0x41E384
            if (shadow[i].frame == 0)
                continue;
            const auto savedIndex = shadowMap.Index(i); W(fd, &savedIndex, 4);
            WritePmmSelfShadowKey(fd, shadowMap.Remap(shadow[i]));               // 0x41E3A1..
        }
    }

    // ---- 10. config2 (0x41E46F..0x41E6E5) --------------------------------
    W(fd, &s->state.modelOutlineColorRed, 4);
    W(fd, &s->state.modelOutlineColorGreen, 4);
    W(fd, &s->state.modelOutlineColorBlue, 4);
    {
        const unsigned char b = s->state.blackBackgroundEnabled != 0;
        W(fd, &b, 1);
    }
    W(fd, &s->state.cameraParentModel, 4);
    W(fd, &s->state.cameraParentBone, 4);
    W(fd, &s->state.cameraAttachmentBasis[0], 4);   // 0xA0438
    for (int i = 0; i < 15; ++i)                               // 0xA043C..A0474
        W(fd, &s->state.cameraAttachmentBasis[1 + i], 4);
    {
        const unsigned char b = s->state.followCameraEnabled != 0;
        W(fd, &b, 1);                                          // 0x9ED98
    }
    {
        const unsigned char b = s->state.cameraAttachmentTransformSuppressed != 0;
        W(fd, &b, 1);                                          // 0xA0478
    }
    {
        const unsigned char b = s->state.floorVisible != 0;
        W(fd, &b, 1);
    }
    {  // 0x22A edit readback on the alt dialog when present (0x41E69E)
        const HWND alt = s->state.floatingWindow;
        text[0] = 0;
        GetWindowTextA(GetDlgItem(alt != nullptr ? alt : main, panel::kGotoFrameEdit), text, 256);
        std::int32_t v = atol(text);
        if (v < 0) v = 0;
        W(fd, &v, 4);                                          // 0x41E6D1
    }
    {
        const unsigned char one = 1;                           // 0x41E6E5
        W(fd, &one, 1);
    }
}


// 11. per-model tail (0x41E70D)
void WritePmmModelTail(int fd, unsigned char** const slots) {
    // ---- 11. per-model tail (0x41E70D) -----------------------------------
    // x64 twin walks slots 0..254 (cmp dl, 0FFh / jb at 0x7FF7CB498DC2).
    for (unsigned char slot = 0; slot < kModelSlotCount; ++slot) {  // loc_41E6F0
        if (slots[slot] == 0) continue;
        W(fd, &slot, 1);
        const std::int32_t registration =
            mdl::Mdl(slots[slot])->frameRegistrationSelection;
        W(fd, &registration, 4);                               // 0x41E72C
    }
}


}  // namespace

namespace { std::atomic<bool> saveInProgress{false}; }
bool SceneSaveInProgress() noexcept { return saveInProgress.load(); }
struct SceneSnapshot {
    std::wstring target;
    bool backup = false;
    bool keepPrevious = false;
    bool numberedManualVersions = false;
    std::vector<unsigned char> bytes;
    pmm_extension::Document extensions;
    mesh_bake_cache::SaveSnapshot meshCache;
    save_compatibility::Report compatibility;
    std::wstring compatibleCopy;
    std::uint64_t revision = 0, generation = 0;
};
static bool CaptureScene(MMDApp* app, const wchar_t* target, bool backup, SceneSnapshot& snapshot) {
    auto* s = app;

    // The scratch CHAR buffer (stack "Text").  The original leaves this
    // uninitialized, so every 0x100 fixed-width path field it feeds carries
    // whatever stack history preceded the save - deterministic in the
    // original's codegen, run-to-run garbage in ours.  Zero-initialize the
    // buffer once: first conversion gets zero tails (a documented deviation
    // - the original's garbage is unreproducible by design), later
    // conversions still carry the previous path's bytes exactly like the
    // original's buffer reuse.  This makes our own back-to-back saves
    // byte-identical, matching the original's determinism property.
    char text[0x100] = {};
    char hdr[0x100];    // sprintf buffer at stack -0x34

    unsigned char** const slots = s->ModelSlots();
    mdl::AccessoryRecord** const accessories = s->AccessorySlots();
    mdl::AccessoryKey** const accTracks = s->AccessoryKeyTracks();
    HWND const main = reinterpret_cast<HWND>(s->Hwnd());

    if (!target || !*target || (!backup && target[0] == L'\\')) {                        // 0x41B097
        if (backup) return false;
        LegacyUiMessageBoxA(main,
                    s->EnglishUI() != 0 ? "Cannot open save file"
                                        : kJpCannotOpenText,
                    "save", 0);
        return false;
    }

    // Check fixed-width fields BEFORE opening/truncating the destination.
    // Neither a substitution character nor a partial multibyte path is usable.
    const wchar_t* invalidPath = nullptr;
    auto checkPath = [&](const wchar_t* path) {
        if (!EncodePmmPath(text, sizeof text, path)) invalidPath = path;
    };
    for (int i = 0; i < kModelSlotCount; ++i)
        if (slots[i]) {
            auto& model = *mdl::Mdl(slots[i]);
            checkPath(model.path);

        }
    for (int i = 0; i < 255; ++i)
        if (accessories[i]) checkPath(accessories[i]->sourcePath);
    checkPath(s->WavePath());
    checkPath(s->AviBackgroundPath());
    checkPath(s->PictureBackgroundPath());
#if MMD_ORIGINAL_PROFILE
    bool identityMismatch = false;
    if (!OriginalPmmKeys(*s)) {
        if (backup) return false;
        runtime_log::Write("ORIGINAL_PROFILE PMM save refused: serialized key slot/link exceeds original bone/morph pool");
        const wchar_t* messages[] = {
            L"保存するキーフレームの番号またはリンクが、元の MMD のボーン・モーフ容量を超えています。PMM を保存できません。既存のファイルは変更していません。",
            L"A saved keyframe slot or link exceeds original MMD's bone/morph capacity. PMM was not saved. The existing file was not changed.",
            L"将保存的关键帧槽位或链索引超出原版 MMD 的骨骼/表情容量，无法兼容保存 PMM。已有文件未改动。"};
        if (main) MessageBoxW(main, messages[s->EnglishUI() <= 2 ? s->EnglishUI() : 1], L"PMM", MB_OK | MB_ICONERROR);
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return false;
    }
    if (invalidPath || !OriginalPmmNames(*s, identityMismatch)) {
        if (backup) return false;
        if (identityMismatch) {
            runtime_log::Write("ORIGINAL_PROFILE PMM save refused: full bone/morph name differs from original raw identity");
            const wchar_t* messages[] = {
                L"ボーンまたはモーフの完全な名前が、元の MMD が照合する短い名前と一致しません。PMM を保存できません。既存のファイルは変更していません。",
                L"A complete bone or morph name differs from the legacy name used by original MMD to identify it. PMM was not saved. The existing file was not changed.",
                L"骨骼或表情的完整名称与原版 MMD 用于匹配的旧名称不同，无法兼容保存 PMM。已有文件未改动。"};
            if (main) MessageBoxW(main, messages[s->EnglishUI() <= 2 ? s->EnglishUI() : 1], L"PMM", MB_OK | MB_ICONERROR);
            SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        } else original_file_encoding::Refuse(main, s->EnglishUI(), L"PMM");
        return false;
    }
#else
    if (invalidPath) {
        if (backup) return false;
        std::wstring message = L"文件路径超出 PMM 的 255 字节限制，无法无损保存。请缩短路径后重试。\n"
                               L"Path exceeds the PMM field size. Shorten it before saving.\n\n";
        message += invalidPath;
        MessageBoxW(main, message.c_str(), L"保存 / Save", MB_OK | MB_ICONERROR);
        return false;
    }
#endif

    const auto compatibility = save_compatibility::InspectPmm(*s);
    if(compatibility.invalidData) {
        runtime_log::WritePath("PMM save refused: invalid keyframe or timeline data",compatibility.reason.c_str());
        SetLastError(ERROR_INVALID_DATA);
        if(backup)return false;
        s->SceneModified()=1;
        const wchar_t* messages[]={
            L"キーフレームの参照またはフレーム範囲が無効なため、PMM を保存できません。PMM v3 に切り替えても修復できません。既存のファイルは変更していません。",
            L"PMM cannot be saved because a keyframe reference or frame range is invalid. Switching to PMM v3 cannot repair it. The existing file was not changed.",
            L"关键帧引用或帧范围无效，无法保存 PMM。切换 PMM v3 也无法修复此问题。已有文件未改动。"};
        if(main)MessageBoxW(main,messages[s->EnglishUI()<=2?s->EnglishUI():1],L"PMM",MB_OK|MB_ICONERROR);
        return false;
    }
#if MMD_ORIGINAL_PROFILE
    if (compatibility.needsNewPmm) {
        if (!backup) original_file_encoding::Refuse(main, s->EnglishUI(), L"PMM");
        return false;
    }
#endif
    pmm_extension::Document extensions;
#if !MMD_ORIGINAL_PROFILE
    try { extensions = pmm_settings::Capture(*s); }
    catch(const std::exception& error) {
        if (backup) return false;
        s->SceneModified()=1;
        const wchar_t* captions[]={L"PMM / アニメーションカーブ",L"PMM / animation curves",L"PMM / 动画曲线"};
        const wchar_t* messages[]={
            L"アニメーションの拡張データを保存できません。既存ファイルは変更していません。\n\n",
            L"Cannot save animation extension data. The existing file was not changed.\n\n",
            L"无法保存动画扩展数据，现有文件未改动。\n\n"};
        const unsigned language=s->EnglishUI()>2?1:s->EnglishUI();
        const int count=MultiByteToWideChar(CP_UTF8,0,error.what(),-1,nullptr,0);
        std::wstring detail(count>0?count:1,L'\0');
        if(count>0)MultiByteToWideChar(CP_UTF8,0,error.what(),-1,detail.data(),count);
        if(!detail.empty()&&detail.back()==L'\0')detail.pop_back();
        const std::wstring message=std::wstring(messages[language])+detail;
        MessageBoxW(main,message.c_str(),captions[language],MB_OK|MB_ICONERROR);
        return false;
    }
    snapshot.meshCache = mesh_bake_cache::CaptureSave(*s);
    if (!pmm_extension::Representable(extensions)) {
        if (backup) return false;
        s->SceneModified() = 1;
        const wchar_t* messages[] = {
            L"PMM 拡張データが形式で表現できるサイズを超えたか、形式が無効です。既存ファイルは変更していません。",
            L"PMM extension data exceeds the representable size or has an invalid format. The existing file was not changed.",
            L"PMM 扩展数据超出文件可表示范围或格式无效，现有文件未改动。"};
        MessageBoxW(main, messages[s->EnglishUI() <= 2 ? s->EnglishUI() : 1],
                    L"PMM", MB_OK | MB_ICONERROR);
        return false;
    }
#endif
    // Persist the original naming stem only for the generated file it belongs to.
    // Save As to a different name starts a fresh series; reopening an increment
    // continues the same series without nesting suffixes.
    std::wstring incrementalTarget;
    snapshot.numberedManualVersions = !backup && pmm_paths::IsProject(target) && autosave::Current().numberedManualVersions;
    if(snapshot.numberedManualVersions) {
        const std::filesystem::path requested=std::filesystem::absolute(target);
        const auto base=save_naming::ProjectStem(requested,extensions);
        const auto next=save_naming::Next(requested.parent_path(),autosave::Current().incrementPattern,base);
        if(next.empty()){SetLastError(ERROR_INVALID_NAME);return false;}
        incrementalTarget=next.wstring();target=incrementalTarget.c_str();
        const auto baseBytes=std::filesystem::path(base).u8string(),fileBytes=next.filename().u8string();
        pmm_extension::Set(extensions,save_naming::BaseKey,{baseBytes.begin(),baseBytes.end()});
        pmm_extension::Set(extensions,save_naming::FileKey,{fileBytes.begin(),fileBytes.end()});
    } else if(!backup) {
        pmm_extension::Erase(extensions,save_naming::BaseKey);
        pmm_extension::Erase(extensions,save_naming::FileKey);
    }
    // Bind ownership to this exact filename; Save/Save As strips the marker.
    pmm_extension::Erase(extensions, autosave::kOwnershipKey);
    if (backup) {
        auto base=save_naming::ProjectStem(app->EnvFileName(),app->PmmProjectSettings().document);
        if(base.empty())base=autosave::Text(app,L"未命名",L"Untitled",L"無題");
        const auto baseBytes=std::filesystem::path(base).u8string();
        const auto name = std::filesystem::path(target).filename().u8string();
        pmm_extension::Set(extensions,save_naming::BaseKey,{baseBytes.begin(),baseBytes.end()});
        pmm_extension::Set(extensions,save_naming::FileKey,{name.begin(),name.end()});
        pmm_extension::Set(extensions, autosave::kOwnershipKey, {name.begin(), name.end()});
    }
    pmmWriteFailed = false;
    snapshot.target = std::filesystem::absolute(target).wstring();
    if (snapshot.target.size() >= kPathFieldCapacity) return false;
    snapshot.backup = backup;
    snapshot.keepPrevious = !backup && pmm_paths::IsProject(target) && autosave::Current().keepPrevious;
    snapshot.compatibility = compatibility;
    struct Sink {
        explicit Sink(std::vector<unsigned char>& bytes) { pmmSnapshotSink = &bytes; }
        ~Sink() { pmmSnapshotSink = nullptr; }
    } sink(snapshot.bytes);
    const int fd = -1;
    WritePmmFileHeader(fd, s, main, hdr, compatibility.needsNewPmm);                   // 1. 0x41B1B3..0x41B355
    WritePmmModelBlocks(fd, slots, text);                   // 2. 0x41B355..0x41C981
    WritePmmCameraSection(fd, s);                           // 3. 0x41C9AE..0x41CF3E
    WritePmmLightSection(fd, s);                            // 4. 0x41CF51..0x41D28F
    WritePmmAccessoryShadowList(fd, main, text);            // 5. 0x41D2AF..0x41D310
    WritePmmAccessoryBlocks(fd, accessories, accTracks, text);  // 6. 0x41D310..0x41DB42
    WritePmmConfigBlock(fd, s, main, text);                 // 7. 0x41DB42..0x41DF7C
    WritePmmSelectionSection(fd, s);                        // 8. 0x41DF7C..0x41E25E
    WritePmmShadowAndConfig2(fd, s, main, text);            // 9+10. 0x41E284..0x41E6E5
    WritePmmModelTail(fd, slots);                           // 11. 0x41E70D
    snapshot.extensions = std::move(extensions);
    snapshot.revision = s->SceneRevision();
    snapshot.generation = s->SceneGeneration();
    return !pmmWriteFailed;
}

// No live scene, window, MME, or graphics access below this boundary.
static bool WriteSnapshot(SceneSnapshot& snapshot) {
    const wchar_t* target = snapshot.target.c_str();
    const bool backup = snapshot.backup;
    auto& extensions = snapshot.extensions;
    if (snapshot.meshCache) {
        std::string error;
        if (!snapshot.meshCache(target, extensions, error)) {
            runtime_log::Write("SAVE mesh cache failed: %s", error.c_str());
            return false;
        }
    }
    int fd = -1;
    wchar_t destination[MAX_PATH]{}, directory[MAX_PATH]{}, temporary[MAX_PATH]{};
    wchar_t* filename = nullptr;
    const DWORD length = GetFullPathNameW(target, MAX_PATH, destination, &filename);
    if (!length || length >= MAX_PATH || !filename) return false;
    wcscpy_s(directory, destination); directory[filename - destination] = L'\0';
    if (!GetTempFileNameW(directory, L"mms", 0, temporary)) return false;
    struct TemporaryFile {
        int& fd; const wchar_t* path;
        ~TemporaryFile() { if (fd >= 0) _close(fd); DeleteFileW(path); }
    } cleanup{fd, temporary};
    if (_wsopen_s(&fd, temporary, 0x8301, 0x40, 0x80)) return false;
    for (std::size_t offset = 0; offset < snapshot.bytes.size();) {
        const auto count = static_cast<unsigned>(std::min<std::size_t>(
            snapshot.bytes.size() - offset, 0x40000000));
        if (_write(fd, snapshot.bytes.data() + offset, count) != static_cast<int>(count)) return false;
        offset += count;
    }
    const auto standardBytes = snapshot.bytes.size();
    const bool written =
#if !MMD_ORIGINAL_PROFILE
        pmm_extension::Append(fd, extensions) &&
#else
        (!backup || pmm_extension::Append(fd, extensions)) &&
#endif
        _commit(fd) == 0;
    const bool closed = _close(fd) == 0; fd = -1;
    if (!written || !closed) return false;
    if(snapshot.numberedManualVersions && GetFileAttributesW(pmm_paths::Effect(destination).c_str())!=INVALID_FILE_ATTRIBUTES)
        return false;
    if (snapshot.keepPrevious && !snapshot.numberedManualVersions) {
        if (!pmm_versions::Publish(temporary, destination)) return false;
    } else if (!MoveFileExW(temporary, destination,
        ((backup || snapshot.numberedManualVersions) ? 0 : MOVEFILE_REPLACE_EXISTING) | MOVEFILE_WRITE_THROUGH)) return false;
#if !MMD_ORIGINAL_PROFILE
    if (!backup && snapshot.compatibility.namesNeedExtension && snapshot.compatibility.canCopyLegacy) {
        try { save_compatibility::CopyPrefix(target, standardBytes, snapshot.compatibleCopy); }
        catch (...) { runtime_log::Write("SAVE compatibility copy failed; complete PMM already saved"); }
    }
#endif
    return true;
}

static void SaveFailure(MMDApp* app) {
    const wchar_t* messages[] = {
        L"PMM ファイルを完全に保存できませんでした。保存先の空き容量とアクセス権を確認してください。",
        L"The PMM file could not be saved completely. Check free space and write access.",
        L"PMM 文件未能完整保存。请检查保存位置的剩余空间和写入权限。"};
    if (app->MainWindow()) MessageBoxW(app->MainWindow(), messages[app->EnglishUI() <= 2 ? app->EnglishUI() : 1],
        L"PMM", MB_OK | MB_ICONERROR);
}

static void CompleteSave(MMDApp* s, SceneSnapshot& snapshot) {
    const wchar_t* target = snapshot.target.c_str();
    // A snapshot may outlive its scene. Never change the replacement scene's
    // filename, dirty flag, settings, or effect assignments on its completion.
    if (s->SceneGeneration() != snapshot.generation) return;
    if (snapshot.backup) { NotifyEffectFileSave(target); return; }
    const bool unchanged = s->SceneRevision() == snapshot.revision;
    wcscpy_s(s->EnvFileName(), kPathFieldCapacity, target);
    if (unchanged) {
#if MMD_ORIGINAL_PROFILE
        s->PmmProjectSettings().document = {};
#else
        s->PmmProjectSettings().document = std::move(snapshot.extensions);
#endif
        s->PmmProjectSettings().processorEdited = false;
        s->PmmProjectSettings().sortingPrecisionEdited = false;
        s->PmmProjectSettings().antialiasEdited = false;
        s->SceneModified() = 0;
    }
    NotifyEffectFileSave(target);
    mmv4mms::ProjectSaved(s, target);
    pmm_io::SetProjectWindowTitle(s->MainWindow(), target);
    try { recent_projects::Record(s, target); }
    catch (const std::exception&) { runtime_log::Write("PMM saved; could not update recent projects"); }
    MessageBeep(0x40);
#if !MMD_ORIGINAL_PROFILE
    save_compatibility::Notify(*s, snapshot.compatibility, snapshot.compatibleCopy,
        snapshot.compatibility.namesNeedExtension && snapshot.compatibility.canCopyLegacy);
#endif
}

static bool SaveSynchronously(MMDApp* app, const wchar_t* target, bool backup) {
    if (saveInProgress.exchange(true)) return false;
    struct Release { ~Release() { saveInProgress.store(false); } } release;
    try {
        SceneSnapshot snapshot;
        if (!CaptureScene(app, target, backup, snapshot)) return false;
        if (!WriteSnapshot(snapshot)) { if (!backup) SaveFailure(app); return false; }
        // Synchronous copies retain the historical contract: their caller owns
        // the effect notification and rotation, including emergency backups.
        if (!backup) CompleteSave(app, snapshot);
        return true;
    } catch (...) { if (!backup) SaveFailure(app); return false; }
}
bool SaveSceneFile(MMDApp* app) { return SaveSynchronously(app, app->EnvFileName(), false); }
bool SaveSceneCopy(MMDApp* app, const wchar_t* target) { return SaveSynchronously(app, target, true); }

namespace background_save {
namespace {
struct Job {
    SceneSnapshot snapshot;
    Options options;
    std::thread worker;
    std::atomic<bool> done{false};
    bool success = false;
    ~Job() { if (worker.joinable()) worker.join(); }
};
std::unique_ptr<Job> job;
Status currentStatus = Status::Saved;
ULONGLONG statusUntil = 0;
bool closePending = false;
std::atomic<bool> backupOverwriting{false};

void Repaint(MMDApp* app) {
    if (!app) return;
    const HWND window = app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow();
    const RECT band = FramePanelLayout(app).topBand;
    if (IsWindow(window)) InvalidateRect(window, &band, FALSE);
}
void Finish(MMDApp* app) {
    if (!job || !job->done.load()) return;
    // Keep the gate held through all main-thread completion hooks and nested
    // modal messages. A callback must not start a second writer here.
    auto finished = std::move(job);
    if (finished->worker.joinable()) finished->worker.join();
    const bool backup = finished->snapshot.backup;
    const bool success = finished->success;
    const bool requestedClose = closePending;
    closePending = false;
    try {
        if (success) CompleteSave(app, finished->snapshot);
        SetStatus(app, success ? (backup ? Status::BackedUp : Status::Saved)
                              : (backup ? Status::BackupFailed : Status::Failed));
        if (finished->options.completed) finished->options.completed(success);
        if (!success && !backup) SaveFailure(app);
    } catch (...) {
        runtime_log::Write("SAVE completion notification failed; disk result=%d", success);
        SetStatus(app, success ? Status::Saved : Status::Failed);
    }
    const bool shouldClose = requestedClose || closePending;
    closePending = false;
    saveInProgress.store(false);
    // A failed manual save must leave the application open, including a
    // previously clean unnamed project. Never implicitly discard it on exit.
    if (shouldClose && (success || backup)) PostMessageW(app->MainWindow(), WM_CLOSE, 0, 0);
}
}

void SetStatus(MMDApp* app, Status value, bool persistent) {
    currentStatus = value;
    statusUntil = persistent ? ~ULONGLONG(0) : GetTickCount64() + 7000;
    if (app && IsWindow(app->MainWindow())) SetTimer(app->MainWindow(), kStatusTimer, 200, nullptr);
    Repaint(app);
}
const wchar_t* StatusText(MMDApp* app) {
    if (!statusUntil) return L"";
    if (currentStatus == Status::BackingUp && backupOverwriting.load()) {
        return autosave::Text(app, L"正在备份，将覆盖旧备份…",
            L"Backing up; replacing oldest backup…",
            L"バックアップ中。古いバックアップを上書きします…");
    }
    // English, Chinese and Japanese share the same status lifetime and layout.
    static const wchar_t* const text[][3] = {
        {L"保存中…", L"Saving…", L"正在保存…"},
        {L"バックアップ中…", L"Backing up…", L"正在备份…"},
        {L"保存中です。お待ちください。", L"Save in progress; please wait", L"已有保存任务正在进行，请稍候。"},
        {L"保存しました", L"Saved", L"保存完成"},
        {L"バックアップしました", L"Backup saved", L"备份完成"},
        {L"古いバックアップを置換しました", L"Old backup replaced", L"备份完成，已覆盖旧备份"},
        {L"保存できませんでした。保存先を確認してください。", L"Save failed. Check the destination.", L"保存失败，请检查保存位置。"},
        {L"バックアップできませんでした。保存先を確認してください。", L"Backup failed. Check the destination.", L"备份失败，请检查备份位置。"},
        {L"バックアップ先が上限に達しました。", L"Backup limit reached; no backup replaced", L"备份数量已满，未覆盖旧备份。"},
        {L"保存完了後に終了します…", L"Saving; will close when done…", L"正在等待保存完成，随后关闭…"}
    };
    return text[static_cast<unsigned>(currentStatus)][app->EnglishUI() <= 2 ? app->EnglishUI() : 1];
}
void ReportBackupOverwrite() noexcept { backupOverwriting.store(true); }
bool Request(MMDApp* app, const wchar_t* destination, Options options) {
    if (!app || !destination || !*destination || app->SceneMutationInProgress()) return false;
    if (GetWindowThreadProcessId(app->MainWindow(), nullptr) != GetCurrentThreadId()) return false;
    if (saveInProgress.exchange(true)) {
        SetStatus(app, Status::Busy, true);
        return false;
    }
    const bool backup = options.backup;
    try {
        backupOverwriting.store(false);
        const auto capturedAt = GetTickCount64();
        auto pending = std::make_unique<Job>();
        pending->options = std::move(options);
        SetStatus(app, backup ? Status::BackingUp : Status::Saving, true);
        if (!CaptureScene(app, destination, backup, pending->snapshot)) {
            saveInProgress.store(false);
            SetStatus(app, backup ? Status::BackupFailed : Status::Failed);
            return false;
        }
        const HWND owner = app->MainWindow();
        runtime_log::Write("SAVE snapshot bytes=%llu elapsed_ms=%llu",
            static_cast<unsigned long long>(pending->snapshot.bytes.size()), GetTickCount64() - capturedAt);
        Job* work = pending.get();
        job = std::move(pending);
        work->worker = std::thread([work, owner] {
            const auto started = GetTickCount64();
            try {
                work->success = (!work->options.beforeWrite || work->options.beforeWrite()) &&
                    WriteSnapshot(work->snapshot) &&
                    (!work->options.afterWrite || work->options.afterWrite());
            } catch (const std::exception& error) {
                runtime_log::Write("SAVE worker failed: %s", error.what());
            } catch (...) { runtime_log::Write("SAVE worker failed with an unknown exception"); }
            runtime_log::Write("SAVE worker result=%d bytes=%llu elapsed_ms=%llu", work->success,
                static_cast<unsigned long long>(work->snapshot.bytes.size()), GetTickCount64() - started);
            work->done.store(true);
            PostMessageW(owner, kComplete, 0, 0);
        });
        return true;
    } catch (...) {
        job.reset();
        saveInProgress.store(false);
        SetStatus(app, backup ? Status::BackupFailed : Status::Failed);
        if (!backup) SaveFailure(app);
        return false;
    }
}
bool HandleMessage(MMDApp* app, UINT message, WPARAM wp) {
    if (message != kComplete && (message != WM_TIMER || wp != kStatusTimer)) return false;
    Finish(app); // Timer is also a fallback if the posted message queue was full.
    if (SceneSaveInProgress() && backupOverwriting.load()) Repaint(app);
    if (statusUntil && GetTickCount64() >= statusUntil && !SceneSaveInProgress()) {
        statusUntil = 0;
        KillTimer(app->MainWindow(), kStatusTimer);
        Repaint(app);
    }
    return true;
}
void Wait(MMDApp* app) {
    if (job && job->worker.joinable()) job->worker.join();
    Finish(app);
}
bool DeferClose(MMDApp* app) {
    if (!SceneSaveInProgress()) return false;
    closePending = true;
    SetStatus(app, Status::WaitingToClose, true);
    return true;
}
} // namespace background_save

}  // namespace mikumikustudio
