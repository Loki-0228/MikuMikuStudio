#include <exception>
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/build_profile.hpp"
// ===========================================================================
// VA 0x00419370 - SaveVmdFile  (0xE23 bytes)
// ===========================================================================
// __thiscall(app, path); port keeps the free-function shape of the dialog
// layer (g_Block).  Two flavours keyed by the edit-mode byte app+0x2F8:
//
//   0x2F8 == 0  MODEL motion - active model (this+0x780[slot @0x910]):
//     counts + min-frame over the three model key arrays (all marks are the
//     occupancy byte at +56 of the 60B bone record, +16 of the 20B morph
//     record, +20 of the 28B IK/display master record; the original counts
//     5 records per 300B/100B/140B group walk - flat scan is the same set).
//   0x2F8 != 0  CAMERA/LIGHT/SELF-SHADOW - the same walk over the global
//     app tables (+0x374 camera 84B / +0x378 light 40B / +0x37C shadow 24B;
//     mark +0x48/+0x24/+0x14).
//
// File layout written (every frame rebased by the min frame of all keys):
//   _wsopen_s(fd, path, 0x8301(_O_BINARY|_O_WRONLY|_O_CREAT|_O_TRUNC),
//             0x40 _SH_DENYNO, 0x80 _S_IREAD)
//   sprintf_s(buf,0x100,"Vocaloid Motion Data 0002"); write(fd,buf,30)
//   name: model mode -> write(model+0x2248, 20); camera mode ->
//         strcpy_s(buf, "カメラ・照明"@0x52BCB8) + write(buf,20)
//   boneCount(4) [0 in camera mode]
//   per bone key: name(15) frame(4) pos(12) quat(16) interp 4x16B
//     (ch0 = u16 x1 + u16 3939-marker + 3 dwords, ch1..3 = 4 dwords; the
//      dword writes overlap the transposed control-byte table exactly as
//      the original - the loader takes the low byte of each)
//   morphCount(4); per morph key: name(15) frame(4) value(4)
//   cameraCount(4); per camera key: frame(4) +0xC(4) +0x10..+0x24(6x4)
//     interp 6x(1B@+0x28+i, 1B@+0x34+i, 1B@+0x2E+i, 1B@+0x3A+i) +0x44(4)
//     view byte (+0x40 != 0)
//   lightCount(4); per light key: frame(4) +0x18/+0x1C/+0x20/+0xC/+0x10/
//     +0x14 (6x4)
//   shadowCount(4); per shadow key: frame(4) +0xC(1) +0x10(4)
//   ikCount(4) [0 in camera mode]; per display key: frame(4), visible(1),
//     model.ikChainCount(4), then each IkChain's bone name(20) and the
//     corresponding DisplayKey::ikStates enabled byte(1)
//   _close.  Open failure -> JP/EN "Cannot save file:%d" box (title "").
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <array>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/accessory_motion_io.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/original_file_encoding.hpp"

namespace mikumikustudio {
namespace {

unsigned char* ActiveModel(MMDApp* app) {
    return app->SelectedModel();
}

const char kCameraLightName[] =  // 0x52BCB8 "カメラ・照明" (byte-exact)
    "\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe";
const char kJpCannotSave[] =  // 0x52BCE4
    "\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xaa\x95\xdb\x91\xb6\x82\xc5\x82"
    "\xab\x82\xdc\x82\xb9\x82\xf1:%d";

#if MMD_ORIGINAL_PROFILE
struct OriginalNames {
    std::array<char, 20> model{};
    std::vector<std::array<char, 20>> bones, morphs;
};

bool PrepareOriginalNames(unsigned char* raw, OriginalNames& names) {
    const auto& model = *mdl::Mdl(raw);
    std::string bytes;
    if (!original_file_encoding::Name(model.name, 20, model.pmxTextBuffers[0], 20, bytes)) return false;
    std::memcpy(names.model.data(), bytes.data(), bytes.size());
    names.bones.resize(model.boneCount);
    names.morphs.resize(model.morphCount);
    const auto prepare = [&](auto& destination, const auto& record, std::size_t maxBytes) {
        if (!original_file_encoding::Name(record.name, 20, record.jpText, maxBytes, bytes)) return false;
        std::memcpy(destination.data(), bytes.data(), bytes.size());
        return true;
    };
    const auto capacity = mdl::BoneKeyCapacity(model);
    for (std::size_t i = 0; i < capacity; ++i) {
        if (!model.boneKeys[i].allocated) continue;
        std::size_t index = i, hops = 0;
        while (index >= model.boneCount && index < capacity && hops++ < capacity)
            index = model.boneKeys[index].previous;
        if (index >= model.boneCount) index = 0;
        if (!model.boneCount || !prepare(names.bones[index], model.boneTable[index], 15)) return false;
    }
    const auto morphCapacity = mdl::MorphKeyCapacity(model.morphKeys);
    for (std::size_t i = 0; i < morphCapacity; ++i) {
        if (!model.morphKeys[i].allocated) continue;
        std::size_t index = i, hops = 0;
        while (index >= model.morphCount && index < morphCapacity && hops++ < morphCapacity)
            index = model.morphKeys[index].previous;
        if (index >= model.morphCount) index = 0;
        if (!model.morphCount || !prepare(names.morphs[index], model.morphs[index], 15)) return false;
    }
    bool display = false;
    for (unsigned i = 0, poolCapacity = static_cast<unsigned>(mdl::KeyPoolCapacity(model.displayKeys)); i < poolCapacity; ++i) display |= model.displayKeys[i].allocated != 0;
    if (display) for (unsigned i = 0; i < model.ikChainCount; ++i) {
        const auto index = model.ikChains[i].boneIndex;
        if (index >= 0 && static_cast<unsigned>(index) < model.boneCount &&
            !prepare(names.bones[index], model.boneTable[index], 20)) return false;
    }
    return true;
}
#endif

}  // namespace

// ---- VA 0x00419370 --------------------------------------------------------
static void SaveVmdFileImpl(const wchar_t* path) {
    MMDApp* app = g_Block;
    if (app == nullptr) return;
    auto& s = *app;

    const bool cameraMode = s.state.optflag[0] != 0;
    unsigned char* const model = ActiveModel(app);
#if MMD_ORIGINAL_PROFILE
    OriginalNames names;
    if (!cameraMode && (!model || !PrepareOriginalNames(model, names))) {
        original_file_encoding::Refuse(app->Hwnd(), s.EnglishUI(), L"VMD");
        return;
    }
#else
    bone_scale_io::PrefixFingerprint prefix;
    extended_names::MotionNames fullNames;
    bool extendedNames = !cameraMode && model && !extended_names::LegacyNamesFit(*mdl::Mdl(model), true);
    extended_names::MotionScope unicodeScope(extendedNames);
    if (extendedNames) {
        const auto& m = *mdl::Mdl(model);
        fullNames.model = text_encoding::ModelName(m, false);
        for (unsigned i=0;i<m.boneCount;++i) fullNames.bones.push_back(text_encoding::BoneName(m.boneTable[i],false));
        for (unsigned i=0;i<m.morphCount;++i) fullNames.morphs.push_back(text_encoding::MorphName(m.morphs[i],false));
    }
#endif
    std::uint64_t standardBytes=0;
    bool writeFailed = false;
    const auto Wr = [&](int descriptor, const void* bytes, unsigned int count) {
        if (_write(descriptor, bytes, count) != static_cast<int>(count)) writeFailed = true;
#if !MMD_ORIGINAL_PROFILE
        else prefix.Append(bytes, count);
#endif
    };

    // ---- pass 1: counts + minimum frame ---------------------------------
    std::uint32_t minFrame = 0xFFFFFFFF;
    unsigned int boneCnt = 0, morphCnt = 0, ikCnt = 0;
    unsigned int camCnt = 0, lightCnt = 0, shadowCnt = 0;

    if (!cameraMode) {
        // Runtime pool capacity, resolved once for this pass.  A flat scan bound
        // to the original 600000 would silently drop every key the pool holds
        // beyond it.
        const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
        const mdl::BoneKey* keys = mdl::BoneKeys(model);
        for (unsigned int i = 0; i < static_cast<unsigned int>(boneKeyCapacity); ++i) {
            if (keys[i].allocated != 0) {
                ++boneCnt;
                const std::uint32_t f = keys[i].frame;
                if (f < minFrame) minFrame = f;
            }
        }
        const mdl::MorphKey* mkeys = mdl::MorphKeys(model);
        const auto morphCapacity1 = mdl::MorphKeyCapacity(mkeys);
        for (unsigned int i = 0; i < morphCapacity1; ++i) {
            if (mkeys[i].allocated != 0) {
                ++morphCnt;
                const std::uint32_t f = mkeys[i].frame;
                if (f < minFrame) minFrame = f;
            }
        }
        const mdl::DisplayKey* ikeys = mdl::DisplayKeys(model);
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(ikeys)); i < poolCapacity; ++i) {
            if (ikeys[i].allocated != 0) {
                ++ikCnt;
                const std::uint32_t f = ikeys[i].frame;
                if (f < minFrame) minFrame = f;
            }
        }
    } else {
        const mdl::CameraKey* cam = app->CameraKeys();
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(cam)); i < poolCapacity; ++i) {
            if (cam[i].selected != 0) {
                ++camCnt;
                const std::uint32_t f = cam[i].frame;
                if (f < minFrame) minFrame = f;
            }
        }
        const mdl::LightKey* light = app->LightKeys();
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(light)); i < poolCapacity; ++i) {
            if (light[i].selected != 0) {
                ++lightCnt;
                const std::uint32_t f = light[i].frame;
                if (f < minFrame) minFrame = f;
            }
        }
        const mdl::SelfShadowKey* shadow = app->ShadowKeys();
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(shadow)); i < poolCapacity; ++i) {
            if (shadow[i].selected != 0) {
                ++shadowCnt;
                const std::uint32_t f = shadow[i].frame;
                if (f < minFrame) minFrame = f;
            }
        }
    }
    // MMS: the selected `.x` keys ride behind the standard sections and share
    // the motion's minimum frame (accessory_motion_io.hpp).
#if !MMD_ORIGINAL_PROFILE
    std::vector<accessory_motion_io::Track> accessoryTracks;
    if (cameraMode)
        accessoryTracks = accessory_motion_io::CaptureSelected(app, minFrame);
#endif

    // ---- open ------------------------------------------------------------
    int fd = -1;
    std::wstring temporary;
    // Stage the complete motion in the destination directory. A failed tail
    // or flush must not replace a previously saved animation.
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        temporary = std::wstring(path) + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
                    L"-" + std::to_wstring(attempt);
        const auto error = _wsopen_s(&fd, temporary.c_str(),
            _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL, _SH_DENYWR, _S_IREAD | _S_IWRITE);
        if (error == 0 || error != EEXIST) break;
    }
    if (fd < 0) {
        char text[256];
        if (s.state.englishUI != 0)
            sprintf_s(text, 0x100, "Cannot save file:%d", errno);
        else
            sprintf_s(text, 0x100, kJpCannotSave, errno);
        LegacyUiMessageBoxA(static_cast<HWND>(s.Hwnd()), text, "", 0);
        return;
    }

    struct TemporaryFile {
        int& fd; const std::wstring& path;
        ~TemporaryFile() { if (fd >= 0) _close(fd); DeleteFileW(path.c_str()); }
    } cleanup{fd, temporary};

    // The 25-byte header text and the 12-byte camera name are both shorter than
    // the fixed 30- and 20-byte writes that follow; the original reads
    // the remaining bytes straight from its stack buffer: sprintf_s (0x14006C6B1)
    // and strcpy_s (0x14006C6E6) store only the text, but _write(fd, buf, 30)
    // (0x14006C6C6) and _write(fd, buf, 20) (0x14006C713) read the padding back out,
    // and the frame is never zeroed first. The header padding was thus
    // uninitialized stack, and the camera-mode name field retained the
    // tail of the header text. Zero initialization gives both paddings
    // deterministic zeros while keeping the write sizes and file layout
    // unchanged.
    // Defect B3-25.
    char buf[256] = {};
    sprintf_s(buf, 0x100, "%s", "Vocaloid Motion Data 0002");
    Wr(fd, buf, 30);                                             // 0x41981E
    if (!cameraMode) {
#if MMD_ORIGINAL_PROFILE
        Wr(fd, names.model.data(), 20);
#else
        Wr(fd, mdl::Mdl(model)->name, 20);                       // 0x419875
#endif
    } else {
        strcpy_s(buf, 0x100, kCameraLightName);
        Wr(fd, buf, 20);                                         // 0x41984F
    }

    // x64 sub_7FF7CB48C450：相机/照明模式（app+808 != 0）下完全不碰 model
    // 指针——模型名取常量"カメラ・照明"，bone/morph/IK 三段直接写 0 计数
    // 并跳过循环（计数全来自全局键表 app+976/984/992）。这些 model 侧的
    // 计数与表指针只在模型模式读取；相机模式下 model 可能为空。
    int boneCount = 0, morphCount = 0, ikChainCount = 0;
    mikumikustudio::mdl::BoneRecord* bones = nullptr;
    mikumikustudio::mdl::MorphRecord* morphs = nullptr;
    mikumikustudio::mdl::IkChain* ikChains = nullptr;
    if (!cameraMode) {
        auto* const rec = mdl::Mdl(model);
        boneCount = rec->boneCount;
        morphCount = rec->morphCount;
        ikChainCount = rec->ikChainCount;
        bones = mikumikustudio::mdl::Bones(model);
        morphs = mikumikustudio::mdl::Morphs(model);
        ikChains = mikumikustudio::mdl::IkChains(model);
    }

    std::uint32_t outDword;
    // ---- bone keys --------------------------------------------------------
    outDword = cameraMode ? 0 : boneCnt;
    Wr(fd, &outDword, 4);                                             // 0x419889
    if (!cameraMode) {
        // Runtime pool capacity, resolved once for the write pass; both the flat
        // scan and the name-resolution walk below are bounded by it.
        const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*mdl::Mdl(model));
        const mdl::BoneKey* keys = mdl::BoneKeys(model);
        for (unsigned int idx = 0; idx < static_cast<unsigned int>(boneKeyCapacity); ++idx) {
            const mdl::BoneKey& rec = keys[idx];
            if (rec.allocated == 0) continue;
            unsigned int boneIdx = idx;                          // 0x4198E0
            // Deliberate hardening: the original walk has no
            // upper bound (0x4198E0 reads keys[boneIdx].previous until the
            // index falls below boneCount), so a previous value read from the
            // file indexes past the key pool, and a cyclic chain never ends.
            // The port stops the walk at the pool capacity and, if no model
            // bone is reached, names the record after the first bone, so
            // the record and the entry count still reach the file.
            // Defect B3-20.
            unsigned int hops = 0;
            while (boneIdx >= static_cast<unsigned int>(boneCount) &&
                   boneIdx < static_cast<unsigned int>(boneKeyCapacity) &&
                   hops++ < static_cast<unsigned int>(boneKeyCapacity))
                boneIdx = keys[boneIdx].previous;
            if (boneIdx >= static_cast<unsigned int>(boneCount))
                boneIdx = 0;
#if MMD_ORIGINAL_PROFILE
            Wr(fd, names.bones[boneIdx].data(), 15);
#else
            if (extendedNames) fullNames.boneOwners.push_back(boneIdx);
            Wr(fd, bones[boneIdx].name, 15);                    // 0x419904
#endif
            outDword = rec.frame - minFrame;
            Wr(fd, &outDword, 4);
            Wr(fd, rec.position, sizeof rec.position);
            Wr(fd, rec.rotation, sizeof rec.rotation);
            // interpolation: 4 channels, transposed control-byte table
            // (+0xC x1 / +0x10 y1 / +0x14 x2 / +0x18 y2, one byte each per
            // channel); ch0 carries the 3939 VMD2 marker as the second u16
            for (int ch = 0; ch < 4; ++ch) {
                if (ch == 0) {                                   // 0x419AA5..
                    Wr(fd, rec.interpolation, 2);
                    const unsigned short marker =
                        rec.physicsDisabled != 0 ? 0x0F63 : 0;
                    Wr(fd, &marker, 2);
                } else {                                         // 0x419AE3
                    Wr(fd, rec.interpolation + ch, 4);
                }
                Wr(fd, rec.interpolation + 4 + ch, 4);
                Wr(fd, rec.interpolation + 8 + ch, 4);
                Wr(fd, rec.interpolation + 12 + ch, 4);
            }
        }
    }

    // ---- morph keys -------------------------------------------------------
    outDword = cameraMode ? 0 : morphCnt;
    Wr(fd, &outDword, 4);                                             // 0x419B8F
    if (!cameraMode) {
        const mdl::MorphKey* mkeys = mdl::MorphKeys(model);
        const auto morphCapacity2 = mdl::MorphKeyCapacity(mkeys);
        for (unsigned int idx = 0; idx < morphCapacity2; ++idx) {
            const mdl::MorphKey& rec = mkeys[idx];
            if (rec.allocated == 0) continue;
            unsigned int morphIdx = idx;                         // 0x419B90..
            // Deliberate hardening: this is the same unbounded traversal as the bone chain
            // above (0x419B90.. reads mkeys[morphIdx].previous until the index
            // falls below morphCount), so a previous value read from the file
            // can index past the morph key pool and a cyclic chain never terminates.
            // The port halts the traversal at the pool capacity and assigns the
            // record the name of the first morph if no model morph is reached.
            // Defect B3-20.
            unsigned int hops = 0;
            const auto morphCapacity3 = mdl::MorphKeyCapacity(mkeys);
            while (morphIdx >= static_cast<unsigned int>(morphCount) &&
                   morphIdx <
                       static_cast<unsigned int>(morphCapacity3) &&
                   hops++ <
                       static_cast<unsigned int>(morphCapacity3))
                morphIdx = mkeys[morphIdx].previous;
            if (morphIdx >= static_cast<unsigned int>(morphCount))
                morphIdx = 0;
#if MMD_ORIGINAL_PROFILE
            Wr(fd, names.morphs[morphIdx].data(), 15);
#else
            if (extendedNames) fullNames.morphOwners.push_back(morphIdx);
            Wr(fd, morphs[morphIdx].name, 15);                  // 0x419C18
#endif
            outDword = rec.frame - minFrame;
            Wr(fd, &outDword, 4);
            Wr(fd, &rec.value, sizeof rec.value);
        }
    }

    // ---- camera keys ------------------------------------------------------
    outDword = camCnt;
    Wr(fd, &outDword, 4);                                             // 0x419E60
    if (camCnt != 0) {
        const mdl::CameraKey* cam = app->CameraKeys();
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(cam)); i < poolCapacity; ++i) {
            const mdl::CameraKey& rec = cam[i];
            if (rec.selected == 0) continue;
            outDword = rec.frame - minFrame;
            Wr(fd, &outDword, 4);
            Wr(fd, &rec.distance, sizeof rec.distance);
            Wr(fd, rec.eye, sizeof rec.eye);
            Wr(fd, rec.target, sizeof rec.target);
            for (int j = 0; j < 6; ++j) {                        // 0x419DAA..
                // was: inner `i`, shadowing the outer record-index i
                Wr(fd, &rec.interpolation[0][j], 1);
                Wr(fd, &rec.interpolation[2][j], 1);
                Wr(fd, &rec.interpolation[1][j], 1);
                Wr(fd, &rec.interpolation[3][j], 1);
            }
            Wr(fd, &rec.fov, sizeof rec.fov);
            const unsigned char view = rec.perspective != 0 ? 1 : 0;
            Wr(fd, &view, 1);
        }
    }

    // ---- light keys -------------------------------------------------------
    outDword = lightCnt;
    Wr(fd, &outDword, 4);                                             // 0x419F4D
    if (lightCnt != 0) {
        const mdl::LightKey* light = app->LightKeys();
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(light)); i < poolCapacity; ++i) {
            const mdl::LightKey& rec = light[i];
            if (rec.selected == 0) continue;
            outDword = rec.frame - minFrame;
            Wr(fd, &outDword, 4);
            Wr(fd, rec.color, sizeof rec.color);
            Wr(fd, rec.direction, sizeof rec.direction);
        }
    }

    // ---- self-shadow keys -------------------------------------------------
    outDword = shadowCnt;
    Wr(fd, &outDword, 4);                                             // 0x419FD6
    if (shadowCnt != 0) {
        const mdl::SelfShadowKey* shadow = app->ShadowKeys();
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(shadow)); i < poolCapacity; ++i) {
            const mdl::SelfShadowKey& rec = shadow[i];
            if (rec.selected == 0) continue;
            outDword = rec.frame - minFrame;
            Wr(fd, &outDword, 4);
            Wr(fd, &rec.mode, sizeof rec.mode);
            Wr(fd, &rec.distance, sizeof rec.distance);
        }
    }

    // ---- IK / display master keys ----------------------------------------
    outDword = cameraMode ? 0 : ikCnt;
    Wr(fd, &outDword, 4);                                             // 0x41A17B..
    if (!cameraMode && outDword != 0) {
        const mdl::DisplayKey* ikeys = mdl::DisplayKeys(model);
        for (unsigned int i = 0, poolCapacity = static_cast<unsigned int>(mdl::KeyPoolCapacity(ikeys)); i < poolCapacity; ++i) {
            const mdl::DisplayKey& rec = ikeys[i];
            if (rec.allocated == 0) continue;
            outDword = rec.frame - minFrame;
            Wr(fd, &outDword, 4);
            const unsigned char visible = rec.visible != 0 ? 1 : 0;
            Wr(fd, &visible, 1);
            outDword = static_cast<std::uint32_t>(ikChainCount);
            Wr(fd, &outDword, 4);
            for (int j = 0; j < ikChainCount; ++j) {             // 0x41A0F0..
                // was: inner `i`, shadowing the outer record-index i
                const int boneIdx = ikChains[j].boneIndex;
#if !MMD_ORIGINAL_PROFILE
                if(extendedNames) fullNames.ikOwners.push_back(boneIdx>=0&&boneIdx<boneCount?std::uint32_t(boneIdx):UINT32_MAX);
#endif
                // Deliberate hardening: the original code writes the chain's bone
                // name directly from bones[boneIdx] without a range check (the
                // IK chain walk at 0x41A0F0), so a model whose chain contains
                // an index outside the bone table reads past the end. This port
                // writes an empty 20-byte name for an out-of-range index so that
                // the record layout remains byte-identical.
                // Defect B3-21.
                if (boneIdx >= 0 && boneIdx < boneCount) {
#if MMD_ORIGINAL_PROFILE
                    Wr(fd, names.bones[boneIdx].data(), 20);
#else
                    Wr(fd, bones[boneIdx].name, 20);
#endif
                } else {
                    char none[20] = {};
                    Wr(fd, none, 20);
                }
                // 0x41A11A: the per-IK display byte array lives in a separate
                // heap buffer pointed to by the record's +0x10 slot (written
                // through the same pointer by the registrar 0x49F8C0) - the
                // on bit is (*(u8**)(rec+0x10))[i], NOT the inline record
                // bytes.  Reading inline made chains >= 4 read the mark byte
                // and the padding/pair-pointer bytes instead (allocation
                // garbage on a fresh load) - toe-IK chains came out 0.
                const unsigned char on =
                    mdl::IkStates(rec)[j] != 0
                        ? 1 : 0;
                Wr(fd, &on, 1);
            }
        }
    }

    standardBytes=static_cast<std::uint64_t>(_telli64(fd));
#if !MMD_ORIGINAL_PROFILE
    // MMS: camera mode appends the selected `.x` keyframes after the standard
    // end, where readers of the standard sections stop.
    if (!writeFailed && extendedNames) {
        const auto block = extended_names::EncodeMotion(fullNames, prefix.bytes(), prefix.crc32());
        Wr(fd, block.data(), static_cast<unsigned int>(block.size()));
    }
    if (!writeFailed && cameraMode && !accessoryTracks.empty()) {
        for (auto& track : accessoryTracks)
            for (auto& key : track.keys) key.frame -= minFrame;
        const std::vector<std::uint8_t> block =
            accessory_motion_io::Encode(accessoryTracks);
        Wr(fd, block.data(), static_cast<unsigned int>(block.size()));
    }

    // The exact morph curves ride in a self-delimiting block written through
    // the same Wr() path as the standard sections, so the bone-scale container
    // behind it still sees the byte count and CRC it recorded, and the standard
    // morph section itself stays what an original reader expects.  A payload
    // that cannot be written fails the save visibly instead of dropping the
    // curves.
    if (!writeFailed && !cameraMode && model != nullptr) {
        std::vector<std::uint8_t> curveBlock;
        std::string curveError;
        if (!morph_curve::CaptureVmd(model, minFrame, curveBlock, curveError)) {
            writeFailed = true;
        } else if (!curveBlock.empty()) {
            Wr(fd, curveBlock.data(), static_cast<unsigned int>(curveBlock.size()));
        }
    }

    if (!writeFailed && !cameraMode && model != nullptr) {
        std::vector<std::uint8_t> block;std::string error;
        if(!bone_curve::CaptureVmd(model,minFrame,block,error)){
            writeFailed=true;
            MessageBoxW(static_cast<HWND>(s.Hwnd()),L"自由曲线的骨骼需要全选关键帧后导出 VMD；原文件未更改。",L"VMD",MB_OK|MB_ICONWARNING);
        }else if(!block.empty())Wr(fd,block.data(),static_cast<unsigned int>(block.size()));
    }
    if (!writeFailed && !cameraMode && model != nullptr) {
        bone_scale_io::VmdExtension extension;
        bone_scale_io::CaptureExtension(model, minFrame, extension);
        extension.prefixBytes = prefix.bytes();
        extension.prefixCrc32 = prefix.crc32();
        writeFailed = bone_scale_io::SaveVmdExtension(fd, extension) == bone_scale_io::ExtensionSave::Failed;
    }
#endif
    if (_commit(fd) != 0) writeFailed = true;
    if (_close(fd) != 0) writeFailed = true;
    fd = -1;
    if (!writeFailed && !MoveFileExW(temporary.c_str(), path,
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        writeFailed = true;
#if !MMD_ORIGINAL_PROFILE
    if(!writeFailed&&extendedNames){
        const auto compatibility=save_compatibility::InspectMotion(*mdl::Mdl(model));std::wstring copy;
        if(compatibility.canCopyLegacy)save_compatibility::CopyPrefix(path,standardBytes,copy);
        save_compatibility::Notify(*app,compatibility,copy,compatibility.canCopyLegacy);
    }
#endif
    if (writeFailed) {
        DeleteFileW(temporary.c_str()); // only the file created exclusively above
        const wchar_t* messages[] = {
            L"VMD を完全に保存できませんでした。既存のファイルは変更されていません。",
            L"The VMD could not be saved completely. The existing file was preserved.",
            L"VMD 未能完整保存，已有文件已保留。"};
        MessageBoxW(static_cast<HWND>(s.Hwnd()),
                    messages[s.state.englishUI <= 2 ? s.state.englishUI : 1],
                    L"VMD", MB_OK | MB_ICONERROR);
    }
}

void SaveVmdFile(const wchar_t* path) {
    try { SaveVmdFileImpl(path); }
    catch (const std::exception&) {
        if (!g_Block) return;
        const wchar_t* messages[] = {L"VMD の保存に必要なメモリを確保できないか、データが無効です。", L"VMD saving failed: insufficient memory or invalid data.", L"VMD 保存失败：内存不足或数据无效。"};
        if (g_Block->MainWindow()) MessageBoxW(g_Block->MainWindow(), messages[g_Block->EnglishUI() <= 2 ? g_Block->EnglishUI() : 1], L"VMD", MB_OK | MB_ICONERROR);
    }
}

}  // namespace mikumikustudio
