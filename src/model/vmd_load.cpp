#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_curve.hpp"
// ===========================================================================
// VA 0x00434B60 - VMD motion file loader  (0x147F bytes)
// ===========================================================================
// __thiscall on the APP block (not the model):
//
//   LoadVmdMotion(app, FileName)   Legacy ANSI-path entry.
//   LoadVmdFile(path)             Unicode entry used by the file dialog and
//                               file drop; preserves the path through open
//                               and extension loading.
//
// Two file flavors share the entry: "Vocaloid Motion Data file" (old,
// 10-byte model name, ver=1) and "Vocaloid Motion Data 0002" (20-byte
// name, ver=2).  app+760 selects the target:
//
//   != 0  CAMERA/LIGHT path - the motion goes to the app's global tracks:
//         bone/morph sections are parsed and discarded, then camera keys
//         (old format expands its single shared 4-byte interpolation
//         curve into all 24 control bytes), light keys (sub_411900) and,
//         for ver 2, self-shadow keys (sub_4120B0).  Tail: ReloadModels +
//         three list refreshers.
//   == 0  MODEL path - model name check against ModelRecord::name (20-byte SJIS;
//         "カメラ・照明" is rejected with its own message, anything else
//         prompts OK/CANCEL), then:
//           * all three key arrays' allocation marks are cleared
//             (bone +56 of kBoneKeyCapacity x60B records, morph +16 of
//             20000x20B, IK master +20 of 1000x28B),
//           * the 30-slot undo/motion ring at model+9964 (28B slots
//             {+0 type=2, +4 zeroed, +12 current frame, +16 ptr to a
//             36B-per-bone initial-pose snapshot, +20 ptr to 192B-per-key
//             raw storage}) advances model+12724 (wraps at 30) and refills
//             the slot; the snapshot copies each bone's +320 position,
//             +332 quaternion and a byte from the model+11672 array,
//           * keyVisitMap (model+14596 x86 / +0x3CAC x64, one byte per
//             bone-key record) is zeroed, sub_4A4940 resets the
//             key allocator,
//           * bone keys: 15-byte name (LF stripped), frame, position,
//             quaternion, then a 4-channel interpolation block - channel
//             0 arrives as two u16s (the second compared against 3939)
//             and channels 1..3 as u32s, each followed by three u32s -
//             registered via sub_49D880 while the "still ok" flag holds,
//           * sub_4A49A0 links/sorts, morph keys via sub_49F190
//             (non-finite values clamped to 0), app+647532 max-frame bump,
//           * three discarded counts, then the model-registration/IK
//             section (frame, visible byte, count <= 10000, 21B records:
//             20 bytes + one flag byte) via sub_49F8C0, freed immediately,
//             max-frame bump again.
//
// Tail for the model path: _close, PanelPaint (0x414610), SelectionReeval
// (0x430510), then SeekModelFrame(model, app+0x980 current frame, app+0xA0CC4
// physics mode) - the frame-seek ported in model_frame_seek.cpp.
//
// The register chain now calls the real (app, rec) overloads of
// 0x410AA0 / 0x411900 / 0x4120B0 defined in src/window/command_control_400.cpp
// (the by-value stack records of the original are passed as pointers -
// documented stub-era deviation absorbed by those ports).  The remaining
// registrar family (0x49D880, 0x49F190, 0x49F8C0, 0x4A49A0, 0x4A4A00) is
// ported in src/model/key_registrars.cpp.
//
// Reference: IDA live disassembly of MikuMikuDance.exe v932 (sole source of
// truth; ../translated/ reference files deviate).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/accessory_motion_io.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include <Windows.h>

#include <cerrno>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <float.h>
#include <io.h>
#include <memory>
#include <share.h>
#include <sys/stat.h>
#include <vector>
#include "vmd_read_guard.hpp"

#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/original_file_encoding.hpp"

namespace mikumikustudio {
// loader-tail helpers, ported in other translation units
void SelectionReeval(MMDApp* app);                   // 0x430510 (ui_selection_reeval.cpp)
void RefreshLightPanel(MMDApp* app);                         // 0x411070 (ui_frame_refresh.cpp)
void RefreshSelfShadowPanel(MMDApp* app);                         // 0x411B90 (ui_frame_refresh.cpp)
void ApplyGravityTrack(MMDApp* app);                        // 0x412330 (track_apply.cpp)

namespace {

int MotionMessageBox(MMDApp* app, const wchar_t* body, const wchar_t* caption,
                     UINT flags) {
    const HWND owner = app->state.hwnd;
    if (!owner) return 0;
    const int result = MessageBoxW(owner, body, caption, flags);
    // The frame loop does not poll keys while a modal dialog owns input.
    // Consume its Enter press so accepting a dropped motion cannot also
    // register the current pose and clear the newly imported key selection.
    if (result != 0) app->state.enterKeyState = 3;
    return result;
}

int MotionMessageBox(MMDApp* app, int language, const wchar_t* japanese,
                     const wchar_t* english, const wchar_t* chinese,
                     UINT flags) {
    const wchar_t* caption = language == 1 ? L"Load motion data"
        : language == 2 ? L"载入动作数据" : L"モーション読込";
    const wchar_t* body = language == 1 ? english
        : language == 2 ? chinese : japanese;
    return MotionMessageBox(app, body, caption, flags);
}

// SJIS constants from .rdata (byte-exact)
const char kCameraLightName[] =  // 0x52BCB8 "カメラ・照明"
    "\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6\x96\xbe";
const char kJpTitleMotionLoad[] =  // 0x52CD58 "モーション読込"
    "\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x83\x66\x81\x5b\x83\x5e"
    "\x93\xc7\x8d\x9e";
const char kJpNotVmd[] =  // 0x52CACC
    "\x82\xb1\x82\xcc\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xcd\x20Vocaloid "
    "Motion Data file \x82\xc5\x82\xcd\x82\xa0\x82\xe8\x82\xdc\x82\xb9"
    "\x82\xf1";
const char kJpCannotOpen[] =  // 0x52CDD4 "...:%d"
    "\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xaa\x93\xc7\x82\xdd\x8d\x9e"
    "\x82\xdf\x82\xdc\x82\xb9\x82\xf1:%d";
const char kJpNoCamLight[] =  // 0x52CCA4
    "\x82\xb1\x82\xcc\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x83\x66"
    "\x81\x5b\x83\x5e\x82\xc9\x82\xcd\x83\x4a\x83\x81\x83\x89\x81\x45"
    "\x8f\xc6\x96\xbe\x82\xcc\x83\x66\x81\x5b\x83\x5e\x82\xcd\x82\xa0"
    "\x82\xe8\x82\xdc\x82\xb9\x82\xf1";
const char kJpIsCamLight[] =  // 0x52CC20
    "\x82\xb1\x82\xcc\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x83\x66"
    "\x81\x5b\x83\x5e\x82\xcd\x83\x4a\x83\x81\x83\x89\x81\x45\x8f\xc6"
    "\x96\xbe\x97p\x82\xcc\x83\x66\x81\x5b\x83\x5e\x82\xc5\x82\xb7\n\n"
    "\x83\x4a\x83\x81\x83\x89\x95\xd2\x8f\x57\x83\x82\x81\x5b\x83\x68"
    "\x82\xc5\x93\xc7\x82\xdd\x8d\x9e\x82\xf1\x82\xc5\x89\xba\x82\xb3"
    "\x82\xa2";
const char kJpNoCamLightShadow[] =  // 0x52CD10
    "\x82\xb1\x82\xcc\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x83\x66"
    "\x81\x5b\x83\x5e\x82\xc9\x82\xcd\x81\x41\x83\x4a\x83\x81\x83\x89"
    "\x81\x45\x8f\xc6\x96\xbe\x81\x45\x83\x5a\x83\x8b\x83\x74\x89\x65"
    "\x82\xcc\x83\x66\x81\x5b\x83\x5e\x82\xcd\x8a\xdc\x82\xdc\x82\xea"
    "\x82\xdc\x82\xb9\x82\xf1";
const char kJpModelMismatch[] =  // 0x52CB48 "... %s ..."
    "\x82\xb1\x82\xcc\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xcd\x20%s\x97"
    "p\x82\xcc\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x83\x66\x81\x5b"
    "\x83\x5e\x82\xc5\x82\xb7\n\x93\xaf\x82\xb6\x83\x7b\x81\x5b\x83\x93"
    "\x96\xbc\x82\xcc\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x82\xcc"
    "\x82\xdd\x82\xf0\x8e\xe6\x82\xe8\x8d\x9e\x82\xde\x8e\x96\x82\xc9"
    "\x82\xc8\x82\xe8\x82\xdc\x82\xb7\n\x91\xb1\x8d\x73\x82\xb5\x82\xdc"
    "\x82\xb7\x82\xa9\x81\x48";

}  // namespace

// ---- VA 0x00434B60 --------------------------------------------------------
static int LoadVmdMotionPath(MMDApp* app, const char* fileName, const wchar_t* wideFileName) {
    if (app == nullptr || (!fileName && !wideFileName)) return 0;
    auto& s = *app;
    unsigned char* const model = app->SelectedModel();

    int fileHandle;
    unsigned int count = 0;  // DstBuf - reused for every section count
    char text[256];
    unsigned char rec[96];
    unsigned int scratch;    // k
    unsigned short word = 0; // v68
    unsigned char flag = 0;

    const HWND hwnd = s.state.hwnd;
    const int language = s.EnglishUI();

    const errno_t openErr = wideFileName
        ? _wsopen_s(&fileHandle, wideFileName,
                    _O_BINARY | _O_RDONLY, _SH_DENYWR, _S_IREAD)
        : _sopen_s(&fileHandle, fileName,
                 _O_BINARY | _O_RDONLY, _SH_DENYWR, _S_IREAD);
    if (openErr != 0) {
        wchar_t message[256]{};
        if (language == 1)
            swprintf_s(message, L"Cannot open file: %d", openErr);
        else if (language == 2)
            swprintf_s(message, L"无法打开文件：%d", openErr);
        else
            swprintf_s(message, L"ファイルを読み込めません：%d", openErr);
        if (hwnd) MotionMessageBox(app, message, language == 1 ? L"Load motion data"
            : language == 2 ? L"载入动作数据" : L"モーション読込", MB_OK | MB_ICONWARNING);
        return 0;
    }

    // No registration or undo changes may happen until every standard record
    // is present. All subsequent standard reads use this immutable snapshot.
    vmd_read::Input input;
    bool valid = false;
    try { valid = input.Load(fileHandle); } catch (const std::exception&) {}
    if (!valid) {
        if (hwnd) MotionMessageBox(app, language,
            L"VMD ファイルが不完全か、レコード数が正しくありません。",
            L"The VMD file is incomplete or contains invalid record counts.",
            L"VMD 文件不完整或记录数量无效。", MB_OK | MB_ICONWARNING);
        _close(fileHandle);
        return 0;
    }
#if MMD_ORIGINAL_PROFILE
    if (!input.OriginalNamesFit()) {
        _close(fileHandle);
        original_import::Refuse(hwnd, language);
        if (hwnd) app->state.enterKeyState = 3;
        return 0;
    }
#endif
    if (s.state.optflag[0] == 0 && model == nullptr) {
        _close(fileHandle);
        return 0;
    }

    std::uint32_t lastImportedFrame=0;
    if(!frame_limits::Add(s.state.currentFrame,input.maximumFrame,lastImportedFrame)){
        if(hwnd)MotionMessageBox(app,language,L"フレーム番号が保存可能な範囲を超えます。",L"The motion exceeds the representable frame range.",L"动作超出可表示的帧号范围，未导入。",MB_OK|MB_ICONWARNING);
        _close(fileHandle);return 0;
    }
    // header + model name (the name read overwrites the header buffer)
    input.Read(text, 30);
    text[30] = 0;
    int ver = 0;
    unsigned int flavor = 0;
    int nameLen;
    if (std::strcmp(text, "Vocaloid Motion Data file") == 0) {
        ver = 1; flavor = 1; nameLen = 10;
    } else if (std::strcmp(text, "Vocaloid Motion Data 0002") == 0) {
        ver = 2; flavor = 2; nameLen = 20;
    } else {
        MotionMessageBox(app, language,
            L"Vocaloid Motion Data ファイルではありません。",
            L"This is not a Vocaloid Motion Data file.",
            L"这不是 Vocaloid Motion Data 动作文件。", MB_OK | MB_ICONWARNING);
        _close(fileHandle);
        return 0;
    }
    input.Read(text, nameLen);
    text[nameLen] = 0;

    // Prepare optional tail data before importing any standard keys. A broken
    // or unknown tail remains ignorable, while allocation/I/O exceptions here
    // still leave the model and global tracks untouched.
#if !MMD_ORIGINAL_PROFILE
    std::vector<accessory_motion_io::Track> accessoryTracks;
    std::vector<std::uint8_t> morphBlock,boneCurveBlock;
    bone_scale_io::VmdExtension scaleExtension;
    bool hasScaleExtension = false;
    extended_names::MotionNames fullNames;
    bool hasFullNames=false;
    std::vector<int> boneTargets,morphTargets,ikTargets;
    std::size_t ikIdentityCursor=0;
    try {
        const auto remaining = input.Size() - input.standardEnd;
        const auto* tail = input.Data(input.standardEnd);
        if (s.state.optflag[0] != 0) {
            if (remaining && remaining <= 64u * 1024 * 1024)
                accessory_motion_io::Decode(tail, remaining, accessoryTracks);
        } else {
            std::size_t tailStart = input.standardEnd;
            const auto nameBytes=extended_names::DecodeMotion(input.Data(0),input.Size(),tailStart,fullNames);
            hasFullNames=nameBytes!=0;tailStart+=nameBytes;
            if(hasFullNames){
                if(fullNames.boneOwners.size()!=input.boneCount||fullNames.morphOwners.size()!=input.morphCount||fullNames.ikOwners.size()!=input.totalIkEntries)
                    throw std::runtime_error("Unicode motion key count mismatch");
                const auto& m=*mdl::Mdl(model);std::vector<std::wstring> names;
                for(unsigned i=0;i<m.boneCount;++i)names.push_back(text_encoding::BoneName(m.boneTable[i],false));
                boneTargets=extended_names::MatchNames(fullNames.bones,names);names.clear();
                for(unsigned i=0;i<m.morphCount;++i)names.push_back(text_encoding::MorphName(m.morphs[i],false));
                morphTargets=extended_names::MatchNames(fullNames.morphs,names);
                ikTargets.reserve(input.maximumIkEntries);
            }
            const auto* nameTail=input.Data(tailStart);
            const auto total = morph_curve::VmdBlockSize(nameTail, input.Size()-tailStart);
            if (total && total <= remaining) {
                std::vector<std::uint8_t> payload;
                if (morph_curve::DecodeVmdBlock(nameTail, total, payload) == total) {
                    morphBlock.assign(nameTail, nameTail + total);
                    tailStart += total;
                }
            }
            const auto boneBytes=bone_curve::VmdBlockSize(input.Data(tailStart),input.Size()-tailStart);
            if(boneBytes){boneCurveBlock.assign(input.Data(tailStart),input.Data(tailStart)+boneBytes);tailStart+=boneBytes;}
            wchar_t widePath[MAX_PATH]{};
            if (!wideFileName) MultiByteToWideChar(CP_ACP, 0, fileName, -1, widePath, MAX_PATH);
            hasScaleExtension = bone_scale_io::LoadVmdExtension(wideFileName ? wideFileName : widePath, scaleExtension,
                static_cast<std::int64_t>(tailStart)) == bone_scale_io::ExtensionLoad::Loaded;
        }
    } catch (const std::exception&) {
        MotionMessageBox(app, language,
            L"モーションの追加データを読み込めませんでした。",
            L"Cannot prepare the motion's extension data.",
            L"无法读取动作扩展数据。", MB_OK | MB_ICONWARNING);
        _close(fileHandle);
        return 0;
    }
#endif

    if (s.state.optflag[0] != 0) {
        auto reserve=[](auto*& keys,std::uint64_t count){
            const auto capacity=mdl::KeyPoolCapacity(keys);std::uint64_t free=0;
            for(std::size_t i=1;i<capacity;++i)if(!keys[i].frame)++free;
            return count<=free||mdl::KeyPoolEnsure(keys,std::uint64_t(capacity)+count-free);
        };
        if(!reserve(app->CameraKeys(),input.cameraCount)||!reserve(app->LightKeys(),input.lightCount)||!reserve(app->ShadowKeys(),input.shadowCount)){
            if(hwnd)MotionMessageBox(app,language,L"メモリ不足です。",L"Not enough memory to import this motion.",L"内存不足，未导入此动作。",MB_OK|MB_ICONWARNING);
            _close(fileHandle);return 0;
        }
        // ==== camera / light / self-shadow motion =========================
        if (ver == 2 && std::memcmp(text, kCameraLightName, 13) != 0) {
            MotionMessageBox(app, language,
                L"このモーションファイルにカメラ・照明・セルフシャドウのデータがありません。",
                L"This motion file has no camera, light, or self-shadow data.",
                L"此动作文件不含镜头、灯光或自阴影数据。", MB_OK | MB_ICONWARNING);
            _close(fileHandle);
            return 0;
        }
        // discard the bone keys: 15-byte name + 24 dwords each
        input.Read(&count, 4);
        for (unsigned int i = 0; i < count; ++i) {
            input.Read(text, 15);
            for (int w = 0; w < 24; ++w) input.Read(&scratch, 4);
        }
        // discard the morph keys: 15 + 4 + 4
        input.Read(&count, 4);
        for (unsigned int j = 0; j < count; ++j) {
            input.Read(text, 15);
            input.Read(&scratch, 4);
            input.Read(&scratch, 4);
        }
        if (input.Read(&count, 4) == 0) {
            MotionMessageBox(app, language,
                L"このモーションファイルにカメラ・照明のデータがありません。",
                L"This motion file has no camera or light data.",
                L"此动作文件不含镜头或灯光数据。", MB_OK | MB_ICONWARNING);
            _close(fileHandle);
            return 0;
        }
        if (ver == 1) {
            unsigned char ok = 1;
            for (unsigned int k = 0; k < count; ++k) {
                std::memset(rec, 0, sizeof rec);
                *reinterpret_cast<int*>(rec + 64) = -1;
                *reinterpret_cast<int*>(rec + 68) = 0;
                input.Read(rec + 0, 4);    // frame
                input.Read(rec + 60, 4);   // distance
                input.Read(rec + 4, 4);    // eye xyz
                input.Read(rec + 8, 4);
                input.Read(rec + 12, 4);
                input.Read(rec + 16, 4);   // target xyz
                input.Read(rec + 20, 4);
                input.Read(rec + 24, 4);
                input.Read(rec + 33, 1);   // shared 4-byte curve
                input.Read(rec + 45, 1);
                input.Read(rec + 39, 1);
                input.Read(rec + 51, 1);
                // x64 @0x7FF7CB48D5A5..0x7FF7CB48D5EF: 每个共享字节只填充
                // 其后的 5 字节（20 次赋值，rec+33/39/45/51 起各成一组
                // 6 字节均匀曲线）；rec[57] 保持清零不被触碰
                for (int c = 1; c <= 5; ++c) {
                    rec[33 + c] = rec[33]; rec[45 + c] = rec[45];
                    rec[39 + c] = rec[39]; rec[51 + c] = rec[51];
                }
                *reinterpret_cast<int*>(rec + 28) = 45;  // fov default
                rec[32] = 0;                             // view flag
                if (ok && !RegisterCameraKey(app, rec)) ok = 0;
            }
        } else {
            unsigned char ok = 1;
            for (unsigned int k = 0; k < count; ++k) {
                std::memset(rec, 0, sizeof rec);
                *reinterpret_cast<int*>(rec + 64) = -1;
                *reinterpret_cast<int*>(rec + 68) = 0;
                input.Read(rec + 0, 4);
                input.Read(rec + 60, 4);
                input.Read(rec + 4, 4);
                input.Read(rec + 8, 4);
                input.Read(rec + 12, 4);
                input.Read(rec + 16, 4);
                input.Read(rec + 20, 4);
                input.Read(rec + 24, 4);
                for (int c = 0; c < 6; ++c) {     // interleaved curve bytes
                    input.Read(rec + c + 33, 1);
                    input.Read(rec + c + 45, 1);
                    input.Read(rec + c + 39, 1);
                    input.Read(rec + c + 51, 1);
                }
                input.Read(rec + 28, 4);   // fov
                input.Read(&flag, 1);
                rec[32] = static_cast<unsigned char>(flag != 0);
                if (ok && !RegisterCameraKey(app, rec)) ok = 0;
            }
        }
        // light keys
        count = 0;
        if (input.lightOffset != vmd_read::Input::absent) {
            input.Seek(input.lightOffset);
            input.Read(&count, 4);
        }
        unsigned char lightOk = 1;
        for (unsigned int k = 0; k < count; ++k) {
            std::memset(rec, 0, sizeof rec);
            input.Read(rec + 0, 4);    // frame
            input.Read(rec + 16, 4);   // color rgb
            input.Read(rec + 20, 4);
            input.Read(rec + 24, 4);
            input.Read(rec + 4, 4);    // position xyz
            input.Read(rec + 8, 4);
            input.Read(rec + 12, 4);
            if (lightOk && !RegisterLightKey(app, rec)) lightOk = 0;
        }
        // self-shadow keys (ver 2 only)
        unsigned char shadowOk = 1;
        if (flavor == 2 && input.shadowOffset != vmd_read::Input::absent) {
            input.Seek(input.shadowOffset);
            input.Read(&count, 4);
            for (unsigned int m = 0; m < count; ++m) {
                std::memset(rec, 0, sizeof rec);
                input.Read(rec + 0, 4);
                input.Read(rec + 4, 1);
                input.Read(rec + 8, 4);
                if (shadowOk && !RegisterSelfShadowKey(app, rec)) shadowOk = 0;
            }
        }
        // MMS: `.x` accessory keys behind the standard end code
        // (accessory_motion_io.hpp).  Anything else there is ignored, as the
        // original reader ignores it.
#if MMD_ORIGINAL_PROFILE
        const bool accessoriesApplied = false;
#else
        const bool accessoriesApplied = !accessoryTracks.empty() &&
            accessory_motion_io::Apply(app, accessoryTracks) > 0;
#endif
        _close(fileHandle);
        PanelPaint(app);
        SelectionReeval(app);
        ReloadModels(app);                              // 0x42E640
        RefreshLightPanel(app);
        RefreshSelfShadowPanel(app);
        if (accessoriesApplied)
            SyncAccessoryEditPanel(app);
        ApplyGravityTrack(app);
        return 0;  // original chains 0x412330's return value
    }

    // ==== model motion =====================================================
#if MMD_ORIGINAL_PROFILE
    std::string modelName;
    const auto& namedModel = *mdl::Mdl(model);
    const bool sameModelName = std::string(namedModel.name, strnlen_s(namedModel.name, 20)) == text ||
        (original_file_encoding::Name(namedModel.name, 20, namedModel.pmxTextBuffers[0], 20, modelName) &&
         modelName == text);
    if (!sameModelName) {
#else
    if (hasFullNames ? fullNames.model != text_encoding::ModelName(*mdl::Mdl(model),false) : std::strcmp(text, mdl::Mdl(model)->name) != 0) {
#endif
        if (std::memcmp(text, kCameraLightName, 13) == 0) {
            MotionMessageBox(app, language,
                L"このモーションはカメラ・照明用です。カメラ編集モードで読み込んでください。",
                L"This is a camera/light motion. Load it in camera editing mode.",
                L"这是镜头/灯光动作，请切换到镜头编辑模式后载入。", MB_OK | MB_ICONWARNING);
            _close(fileHandle);
            return 0;
        }
        std::wstring sourceModelName;
        if (!text_encoding::Decode(text, 932, sourceModelName))
            sourceModelName = L"?";
        wchar_t prompt[512]{};
        if (language == 1)
            swprintf_s(prompt, L"This motion file is for '%s'.\nOnly matching bone names will be imported.\nContinue?", sourceModelName.c_str());
        else if (language == 2)
            swprintf_s(prompt, L"此动作文件属于“%s”。\n只会导入骨骼名称匹配的动作。\n是否继续？", sourceModelName.c_str());
        else
            swprintf_s(prompt, L"このモーションファイルは「%s」用です。\n同じボーン名のモーションのみ読み込みます。\n続行しますか？", sourceModelName.c_str());
        if (!hwnd || MotionMessageBox(app, prompt,
                        language == 1 ? L"Load motion data"
                            : language == 2 ? L"载入动作数据" : L"モーション読込",
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            _close(fileHandle);
            return 0;
        }
    }
    input.Read(&count, 4);
    const unsigned int boneKeyCount = count;

    // Prepare loader-owned allocations before clearing selections or replacing
    // an undo slot. The per-record scratch also cannot fail midway through IK.
    mdl::ModelRecord& record = *mdl::Mdl(model);
    std::size_t poseBytes = 0, undoBytes = 0;
    const bool sizesFit = vmd_read::Input::AllocationSize(record.boneCount,
        sizeof(mdl::BonePoseSnapshot), poseBytes) &&
        vmd_read::Input::AllocationSize(boneKeyCount, 192, undoBytes);
    using Allocation = std::unique_ptr<unsigned char, decltype(&std::free)>;
    Allocation preparedPose(sizesFit && poseBytes ? static_cast<unsigned char*>(std::malloc(poseBytes)) : nullptr, &std::free);
    Allocation preparedKeys(sizesFit && undoBytes ? static_cast<unsigned char*>(std::malloc(undoBytes)) : nullptr, &std::free);
    std::vector<unsigned char> ikEntries;
#if MMD_ORIGINAL_PROFILE
    // Complete standard names can differ from the PMX loader's NUL-reserving
    // legacy mirrors. Prepare the translation before changing undo or keys;
    // the registrar itself deliberately keeps its original raw-name contract.
    std::vector<std::array<char, 40>> ikNameAliases;
#endif
    bool allocationsReady = sizesFit && (!poseBytes || preparedPose) && (!undoBytes || preparedKeys);
    if (allocationsReady) {
        // Keep an extra terminator beyond the fixed-width entry array.
        try {
            ikEntries.resize(std::size_t(input.maximumIkEntries) * 21 + 1);
#if MMD_ORIGINAL_PROFILE
            for (unsigned i = 0; i < record.ikChainCount; ++i) {
                const int index = record.ikChains[i].boneIndex;
                if (index < 0 || static_cast<unsigned>(index) >= record.boneCount) continue;
                const auto& bone = record.boneTable[index];
                std::string canonical;
                if (!original_file_encoding::Name(bone.name, 20, bone.jpText, 20, canonical)) continue;
                std::array<char, 40> alias{};
                std::memcpy(alias.data(), canonical.data(), canonical.size());
                std::memcpy(alias.data() + 20, bone.name, 20);
                if (std::memcmp(alias.data(), alias.data() + 20, 20) != 0) ikNameAliases.push_back(alias);
            }
#endif
        }
        catch (const std::exception&) { allocationsReady = false; }
    }
    if(allocationsReady){
        auto needed=[](const auto* keys,std::size_t capacity,std::size_t heads,std::uint64_t incoming){
            std::uint64_t free=0;for(std::size_t i=heads;i<capacity;++i)if(!keys[i].frame)++free;
            return std::uint64_t(capacity)+(incoming>free?incoming-free:0);
        };
        allocationsReady=mdl::BoneKeyPoolEnsure(record,needed(record.boneKeys,mdl::BoneKeyCapacity(record),record.boneCount,input.boneCount))&&
            mdl::MorphKeyPoolEnsure(record,needed(record.morphKeys,mdl::MorphKeyCapacity(record),record.morphCount,input.morphCount))&&
            mdl::DisplayKeyPoolEnsure(record,needed(record.displayKeys,mdl::KeyPoolCapacity(record.displayKeys),1,input.displayCount));
    }
    if (!allocationsReady) {
        MotionMessageBox(app, language,
            L"モーションを読み込むためのメモリが不足しています。",
            L"Not enough memory to load this motion.",
            L"内存不足，无法载入此动作。", MB_OK | MB_ICONWARNING);
        _close(fileHandle);
        return 0;
    }
    if (poseBytes) std::memset(preparedPose.get(), 0, poseBytes);
    if (undoBytes) std::memset(preparedKeys.get(), 0, undoBytes);

    // clear the allocation marks of all three key arrays
    mikumikustudio::mdl::BoneKey* const bkeys = mikumikustudio::mdl::BoneKeys(model);
    mikumikustudio::mdl::MorphKey* const mkeys = mikumikustudio::mdl::MorphKeys(model);
    mikumikustudio::mdl::DisplayKey* const ikeys = mikumikustudio::mdl::DisplayKeys(model);
    // The capacity is resolved once, outside the sweep: a per-iteration lookup
    // would take the registry lock for every slot.
    const int boneKeyCapacity = static_cast<int>(mdl::BoneKeyCapacity(*mdl::Mdl(model)));
    for (int i = 0; i < boneKeyCapacity; ++i) bkeys[i].allocated = 0;
    const auto morphCapacity1 = mdl::MorphKeyCapacity(mkeys);
    for (int i = 0; i < morphCapacity1; ++i) mkeys[i].allocated = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(ikeys); i < capacity; ++i) ikeys[i].allocated = 0;

    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
    record.undoDirty = 1;
    record.redoDirty = 0;
    std::uint32_t& ringIdx = record.undoState[0];
    if (++ringIdx >= 30) ringIdx = 0;
    record.undoState[1] = ringIdx;
#if !MMD_ORIGINAL_PROFILE
    bone_scale::CaptureUndo(model);
#endif

    auto& undo = mikumikustudio::mdl::Mdl(model)->undoRings[0].slots[ringIdx];
    undo.operation = 2;
    undo.dirty = 0;
    undo.frame = s.state.currentFrame;
    if (undo.bonePose != nullptr)
        free(undo.bonePose);
    const int boneCnt = static_cast<int>(record.boneCount);
    // x64 @0x7FF7CB48DE70..0x7FF7CB48E013: 每骨骼一个 36 字节槽位
    // (boneIndex / trans / rotQuat / 物理标志)，源端等价于按
    // BoneRecord 类型化字段读取（x64 骨骼表 624 字节 stride 下
    // trans@+328、rotQuat@+340），物理标志取 model+12584[b]
    auto* const snap = reinterpret_cast<mdl::BonePoseSnapshot*>(preparedPose.release());
    undo.bonePose = snap;
    if (boneCnt > 0) {
        mikumikustudio::mdl::BoneRecord* const bones =
            mikumikustudio::mdl::Bones(model);
        const unsigned char* const flags =
            mikumikustudio::mdl::Mdl(model)->bonePhysicsState;
        for (int b = 0; b < boneCnt; ++b) {
            mikumikustudio::mdl::BonePoseSnapshot& slot = snap[b];
            slot.boneIndex = b;
            std::memcpy(slot.position, bones[b].trans, sizeof slot.position);
            std::memcpy(slot.rotation, bones[b].rotQuat, sizeof slot.rotation);
            slot.physicsDisabled = flags[b];
        }
    }
    if (undo.auxiliaryPose != nullptr)
        free(undo.auxiliaryPose);
    unsigned char* const rawKeys = preparedKeys.release();
    undo.auxiliaryPose = rawKeys;
    mdl::BoneKeyVisitClear(*mdl::Mdl(model));
    ResetBoneKeyCursor(model);
    (void)rawKeys;  // consumed by the registrar family (0x49D880 et al.)

    // bone keys
    //
    // MMS addition: this import replaces the keyframes it registers, so a key
    // it overwrites takes the default LINEAR scale curve instead of keeping one
    // a previous session left on that slot.  bone_scale_io::ApplyImportedExtension
    // runs afterwards and writes the curves a v2 tail carries (a v1 tail or no
    // tail leaves the default).  The scope touches nothing else: the standard
    // VMD body, the morph block and the bone-scale container are unchanged.
#if !MMD_ORIGINAL_PROFILE
    bone_scale::DefaultCurveScope defaultScaleCurve;
#endif
    unsigned char boneOk = 1;
    for (unsigned int k = 0; k < boneKeyCount; ++k) {
        input.Read(text, 15);
        text[15] = 0;
        if (char* nl = std::strchr(text, 10)) *nl = 0;
        strcpy_s(reinterpret_cast<char*>(rec), 30, text);
        input.Read(rec + 32, 4);   // frame
        input.Read(rec + 52, 4);   // position
        input.Read(rec + 56, 4);
        input.Read(rec + 60, 4);
        input.Read(rec + 36, 4);   // quaternion
        input.Read(rec + 40, 4);
        input.Read(rec + 44, 4);
        input.Read(rec + 48, 4);
        for (int ch = 0; ch < 4; ++ch) {
            if (ch != 0) {
                input.Read(&scratch, 4);
                rec[ch + 65] = static_cast<unsigned char>(scratch);
            } else {
                input.Read(&word, 2);
                rec[65] = static_cast<unsigned char>(word);
                input.Read(&word, 2);
                rec[64] = static_cast<unsigned char>(word == 3939);
            }
            input.Read(&scratch, 4);
            rec[ch + 69] = static_cast<unsigned char>(scratch);
            input.Read(&scratch, 4);
            rec[ch + 73] = static_cast<unsigned char>(scratch);
            input.Read(&scratch, 4);
            rec[ch + 77] = static_cast<unsigned char>(scratch);
        }
        if (boneOk) {
#if !MMD_ORIGINAL_PROFILE
            if(hasFullNames){
                const int destination=boneTargets[fullNames.boneOwners[k]];
                if(destination<0)continue;
                const int previousSelection=record.selectedBone;
                record.selectedBone=destination;
                const bool registered=RegisterBoneKey(model,rec,s.state.currentFrame,1);
                record.selectedBone=previousSelection;
                if(!registered)boneOk=0;
                continue;
            }
#endif
            if (!RegisterBoneKey(model, rec,
                           s.state.currentFrame, 0))
                boneOk = 0;
        }
    }
    ResetMorphKeyCursor(model);

    // morph keys
    input.Read(&count, 4);
    unsigned char morphOk = 1;
    for (unsigned int k = 0; k < count; ++k) {
        input.Read(text, 15);
        text[15] = 0;
        if (char* nl = std::strchr(text, 10)) *nl = 0;
        strcpy_s(reinterpret_cast<char*>(rec), 30, text);
        input.Read(rec + 32, 4);   // frame
        input.Read(rec + 36, 4);   // value
        if (!_finite(*reinterpret_cast<float*>(rec + 36)))
            *reinterpret_cast<float*>(rec + 36) = 0.0f;
        if (morphOk) {
#if !MMD_ORIGINAL_PROFILE
            if(hasFullNames){
                const int destination=morphTargets[fullNames.morphOwners[k]];
                if(destination<0)continue;
                std::uint32_t relative;float value;
                std::memcpy(&relative,rec+32,4);std::memcpy(&value,rec+36,4);
                if(!RegisterMorphKeyAtIndex(model,destination,relative+std::uint32_t(s.state.currentFrame),value))morphOk=0;
                continue;
            }
#endif
            if (!RegisterMorphKeyFromRecord(model, rec,
                           s.state.currentFrame))
                morphOk = 0;
        }
    }
    unsigned int modelMax = record.maxFrame;
    if (s.LastRegisteredFrame() < modelMax)
        s.LastRegisteredFrame() = modelMax;

    // Skip the complete camera/light/shadow records, not just their counts.
    // Their validated lengths determine where the optional IK section begins.
    if (input.displayOffset != vmd_read::Input::absent) {
        input.Seek(input.displayOffset);
        input.Read(&count, 4);
        ResetDisplayKeyCursor(model);
        unsigned char ikOk = 1;
        for (unsigned int k = 0; k < count; ++k) {
            input.Read(rec + 0, 4);   // frame
            input.Read(&flag, 1);     // visible
            rec[4] = static_cast<unsigned char>(flag != 0);
            input.Read(rec + 8, 4);   // IK entry count
            int n = *reinterpret_cast<int*>(rec + 8);
            unsigned char* entries = n > 0 ? ikEntries.data() : nullptr;
            for (int e = 0; e < n; ++e) {
                input.Read(entries + 21 * e, 20);
#if MMD_ORIGINAL_PROFILE
                for (const auto& alias : ikNameAliases) {
                    if (std::strncmp(reinterpret_cast<const char*>(entries + 21 * e), alias.data(), 20) == 0) {
                        std::memcpy(entries + 21 * e, alias.data() + 20, 20);
                        break;
                    }
                }
#endif
                input.Read(&flag, 1);
                entries[21 * e + 20] = static_cast<unsigned char>(flag != 0);
            }
            if (entries) entries[21 * n] = 0;
            *reinterpret_cast<int*>(rec + 16) = 0;
#if !MMD_ORIGINAL_PROFILE
            struct RestoreIkTargets { const std::vector<int>* previous=extended_names::importedIkTargets;
                ~RestoreIkTargets(){extended_names::importedIkTargets=previous;} } restoreIkTargets;
            if(hasFullNames){
                ikTargets.clear();
                for(unsigned e=0;e<static_cast<unsigned>(n);++e){const auto owner=fullNames.ikOwners[ikIdentityCursor++];ikTargets.push_back(owner==UINT32_MAX?-1:boneTargets[owner]);}
                extended_names::importedIkTargets=&ikTargets;
            }
#endif
            if (ikOk) {
                if (!RegisterDisplayKeyFromRecord(model, *reinterpret_cast<int*>(rec), rec[4],
                               n, entries, 0, nullptr,
                               s.state.currentFrame))
                    ikOk = 0;
            }
        }
        modelMax = record.maxFrame;
        if (s.LastRegisteredFrame() < modelMax)
            s.LastRegisteredFrame() = modelMax;
    }

    // The prepared extensions apply only after the standard keys they refer
    // to exist. These model-facing APIs retain their own allocation policy.
#if !MMD_ORIGINAL_PROFILE
    extended_names::MotionScope unicodeScope(false, hasFullNames ? &boneTargets : nullptr, hasFullNames ? &morphTargets : nullptr);
    if (!morphBlock.empty())
        morph_curve::ApplyVmd(model, morphBlock.data(), morphBlock.size(),
                              static_cast<int>(s.state.currentFrame));
    if (hasScaleExtension)
        bone_scale_io::ApplyImportedExtension(model, scaleExtension, s.state.currentFrame);
    if(!boneCurveBlock.empty()){
        std::string error;
        if(!bone_curve::ApplyVmd(model,boneCurveBlock.data(),boneCurveBlock.size(),int(s.state.currentFrame),error))
            MotionMessageBox(app,language,L"自由カーブは空のボーントラックに読み込んでください。標準キーのみ読み込みました。",L"Free curves require empty bone tracks. Only the standard keys were imported.",L"自由曲线需要导入空的骨骼轨道，本次只读取了标准关键帧。",MB_OK|MB_ICONWARNING);
    }
#endif

    _close(fileHandle);
    PanelPaint(app);                                    // 0x414610
    SelectionReeval(app);                               // 0x430510
    return SeekModelFrame(model, s.state.currentFrame,
                     s.PlaybackPhysicsMode());
}

int LoadVmdMotion(MMDApp* app, const char* fileName) {
    return LoadVmdMotionPath(app, fileName, nullptr);
}

// Keep dialog/drop paths in Unicode, as model loading does. An ANSI roundtrip
// loses characters that are not available in the system code page.
void LoadVmdFile(const wchar_t* path) {
    if (g_Block == nullptr) return;
    LoadVmdMotionPath(g_Block, nullptr, path);
}

}  // namespace mikumikustudio
