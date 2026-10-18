// ===========================================================================
// VA 0x00418750 - SaveVpdFile  (0x2B2 bytes)
// VA 0x00418A10 - LoadVpdFile  (0x95C bytes)
// ===========================================================================
// Both are __thiscall(app, path) on the app block; the port keeps the
// free-function shape used by the file-dialog command layer (g_Block), the
// same convention as LoadVmdFile (src/model/vmd_load.cpp).
//
// SAVE (0x418750): _wfopen(path, L"wt") then a fixed fprintf sequence with
// byte-exact .rdata formats (0x52BBDC..0x52BC6C):
//   "Vocaloid Pose Data file\n\n"
//   "%s.osm;\t\t// <SJIS 親ファイル名>"   arg = model+0x2248 name
//   "%d;\t\t\t\t// <SJIS 総ポーズボーン数>" arg = count of selected bones
//   per selected bone i (ModelRecord selection bitmap/count/BoneRecord table):
//     "Bone%d{%s\n"      running selected index, bone name at record +0
//     pose source pick (0x418860): bone->hasRigidBody != 0 && bone->physicsDisabled == 0 &&
//         physMode(app+0xA0CC4) >= 2  ->  physics pose
//         (+0x188 trans / +0x194 quat), else raw pose
//         (+0x140 trans / +0x14C quat)
//     "  %f,%f,%f;\t\t\t\t// trans x,y,z\n"
//     "  %f,%f,%f,%f;\t\t// Quaternion x,y,z,w\n"
//     "}\n\n"
//   fclose.  (floats are promoted to double by varargs, as in the original)
//
// LOAD (0x418A10): _wfopen_s(&fp, path, L"r"); 3 discarded fgets (header,
// blank, model line); fscanf(fp, "%d;", &count); 2 more discarded fgets
// (count-line comment tail + blank).  UI: EnableWindow(400,true)/
// EnableWindow(401,false) like the VMD loader.  Undo ring at model+0x31B4
// (30 slots of 0x1C at model+0x26EC; slot+0 = type 1 (pose), slot+4 = count,
// slot+16 = new(0x24*count) snapshot {int boneIdx, float3 pos, quat, flag}).
// Per VPD record: fgets(name line), strstr "{", sprintf_s(name),
// strchr(name,'\n')->0, fscanf "  %f,%f,%f;" (trans), fgets (comment tail),
// fscanf "  %f,%f,%f,%f;" (quat), then 3 discarded fgets (comment/}/blank).
// Name matched byte-wise against every model bone; on hit: snapshot the old
// pose into undo, twist-correct type-8 / type-4+0x400 bones exactly like the
// key registrar 0x49D880 (axis = PMX bone->axis / PMD tail-bone rest delta),
// write trans -> bone->trans, quat -> bone->rotQuat, and set both the
// selection and physics-state bytes to 1.
//
// MMS addition: native bone scale in a tail extension (vpd_scale_io.hpp).
// The standard text and the position where the original reader stops are
// untouched - the writer keeps the byte-exact fprintf sequence above and
// appends the extension after it, the loader reads it only after the standard
// segment has been parsed.  A VPD without the tail keeps the stock behavior
// (every bone the pose touches falls back to the default identity scale, so a
// load never inherits the previous pose's residual scale), and a tail that
// fails any of its checks is refused as a whole with a runtime log entry.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/globals.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/vpd_scale_io.hpp"
#include "mikudancestudio/original_file_encoding.hpp"
#include "mikudancestudio/original_import.hpp"

namespace mikudancestudio {
namespace {

// Active model = slot array at this+0x780 indexed by byte this+0x910.
unsigned char* ActiveModel(MMDApp* app) {
    return app->SelectedModel();
}

// ---- byte-exact .rdata strings --------------------------------------------
const char kHdr[] = "Vocaloid Pose Data file\n\n";               // 0x52BC6C
const char kModelFmt[] =                                         // 0x52BC50
    "%s.osm;\t\t// \x90\x65\x83\x74\x83\x40\x83\x43\x83\x8b\x96\xbc\n";
const char kCountFmt[] =                                         // 0x52BC30
    "%d;\t\t\t\t// \x91\x8d\x83\x7c\x81\x5b\x83\x59\x83\x7b\x81\x5b\x83"
    "\x93\x90\x94\n\n";
const char kBoneFmt[] = "Bone%d{%s\n";                           // 0x52BC24
const char kTransFmt[] =                                         // 0x52BC04
    "  %f,%f,%f;\t\t\t\t// trans x,y,z\n";
const char kQuatFmt[] =                                          // 0x52BBDC
    "  %f,%f,%f,%f;\t\t// Quaternion x,y,z,w\n";
const char kCloseFmt[] = "}\n\n";                                // 0x52BBD8
const wchar_t kModeWrite[] = L"wt";                              // 0x52BC88
const wchar_t kModeRead[] = L"r";                                // 0x52BCB4
const char kCountScan[] = "%d;";                                 // 0x52BCB0
const char kBrace[] = "{";                                       // 0x52BCAC
const char kTransScan[] = "  %f,%f,%f;";                         // 0x52BCA0
const char kQuatScan[] = "  %f,%f,%f,%f;";                       // 0x52BC90

}  // namespace

// ---- VA 0x00418750 --------------------------------------------------------
void SaveVpdFile(const wchar_t* path) {
    MMDApp* app = g_Block;
    if (app == nullptr) return;
    unsigned char* const model = ActiveModel(app);
    if (model == nullptr) return;

    mdl::ModelRecord& modelRecord = *mdl::Mdl(model);
    FILE* fp = nullptr;
#if MMD_ORIGINAL_PROFILE
    std::string modelName;
    std::vector<std::string> boneNames(modelRecord.boneCount);
    bool valid = original_file_encoding::Name(modelRecord.name, 20,
        modelRecord.pmxTextBuffers[0], 20, modelName) && modelName.find_first_of("\r\n") == std::string::npos;
    for (unsigned i = 0; valid && i < modelRecord.boneCount; ++i) {
        if (!modelRecord.boneSelection[i]) continue;
        const auto& bone = modelRecord.boneTable[i];
        valid = original_file_encoding::Name(bone.name, 20, bone.jpText, 20, boneNames[i]) &&
                boneNames[i].find_first_of("\r\n") == std::string::npos;
    }
    if (!valid) {
        original_file_encoding::Refuse(app->Hwnd(), app->EnglishUI(), L"VPD");
        return;
    }
    std::wstring temporary;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        temporary = std::wstring(path) + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(attempt);
        const auto error = _wsopen_s(&fd, temporary.c_str(),
            _O_TEXT | _O_WRONLY | _O_CREAT | _O_EXCL, _SH_DENYWR, _S_IREAD | _S_IWRITE);
        if (error != EEXIST) break;
    }
    if (fd < 0) return;
    fp = _wfdopen(fd, L"wt");
    if (!fp) { _close(fd); DeleteFileW(temporary.c_str()); return; }
#else
    if (_wfopen_s(&fp, path, kModeWrite) != 0 || fp == nullptr) return;
#endif

    std::fprintf(fp, "%s", kHdr);
#if MMD_ORIGINAL_PROFILE
    std::fprintf(fp, kModelFmt, modelName.c_str());
#else
    std::fprintf(fp, kModelFmt, modelRecord.name);
#endif

    const int boneCnt = modelRecord.boneCount;
    unsigned char* const selBits = modelRecord.boneSelection;
    int selected = 0;
    for (int i = 0; i < boneCnt; ++i)
        if (selBits[i] != 0) ++selected;
    std::fprintf(fp, kCountFmt, selected);

    mikudancestudio::mdl::BoneRecord* const bones = mikudancestudio::mdl::Bones(model);
    const int physMode = app->PlaybackPhysicsMode();
    // MMS addition: the pose scales of the exported bones ride in the tail
    // extension (vpd_scale_io.hpp).  One entry per selected bone whose pose
    // scale is a real, non-identity matrix; the identity scale is the default
    // every loaded bone falls back to anyway.
#if !MMD_ORIGINAL_PROFILE
    std::vector<vpd_scale_io::Entry> scaleEntries;
#endif
    int running = 0;
    for (int i = 0; i < boneCnt; ++i) {
        if (selBits[i] == 0) continue;
        mikudancestudio::mdl::BoneRecord* const bone = bones + i;
#if MMD_ORIGINAL_PROFILE
        std::fprintf(fp, kBoneFmt, running, boneNames[i].c_str());
#else
        std::fprintf(fp, kBoneFmt, running, bone->name);
#endif
        ++running;
        // 0x418860: physics-pose source only for flagged bones with the
        // physics mode >= 2; raw kinematic pose otherwise.
        const float* trans;
        const float* quat;
        if (bone->hasRigidBody != 0 && bone->physicsDisabled == 0 && physMode >= 2) {
            trans = bone->ikBackup;
            quat = bone->ikBackup + 3;
        } else {
            trans = bone->trans;
            quat = bone->rotQuat;
        }
        std::fprintf(fp, kTransFmt, trans[0], trans[1], trans[2]);
        std::fprintf(fp, kQuatFmt, quat[0], quat[1], quat[2], quat[3]);
        std::fprintf(fp, "%s", kCloseFmt);

#if !MMD_ORIGINAL_PROFILE
        const bone_scale::Matrix scale = bone_scale::Pose(model, i);
        if (bone_scale::Valid(scale) && !bone_scale::Identity(scale)) {
            vpd_scale_io::Entry entry;
            entry.bone = i;
            std::memcpy(entry.name.data(), bone->name, entry.name.size());
            entry.scale = scale;
            scaleEntries.push_back(entry);
        }
#endif
    }
#if MMD_ORIGINAL_PROFILE
    bool written = !std::ferror(fp) && std::fflush(fp) == 0 && _commit(_fileno(fp)) == 0;
    if (std::fclose(fp) != 0) written = false;
    if (!written || !MoveFileExW(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        runtime_log::Write("ORIGINAL_PROFILE VPD save failed: existing file preserved");
        if (app->Hwnd()) MessageBoxW(static_cast<HWND>(app->Hwnd()),
            L"VPD could not be saved completely. Existing file preserved.\nVPD 未能完整保存，已有文件已保留。",
            L"VPD", MB_OK | MB_ICONERROR);
    }
#else
    std::fclose(fp);
    // The standard text above is complete and unchanged at this point; the
    // extension is appended after it, behind the reader's end position.  With
    // no scale to export no tail is written at all.
    if (vpd_scale_io::AppendTail(path, scaleEntries) == vpd_scale_io::TailSave::Failed)
        runtime_log::Write(
            "bone scale: the VPD scale extension could not be appended to the "
            "pose file; the saved pose carries no scale");
#endif
}

// ---- VA 0x00418A10 --------------------------------------------------------
void LoadVpdFile(const wchar_t* path) {
    MMDApp* app = g_Block;
    if (app == nullptr) return;
    unsigned char* const model = ActiveModel(app);
    if (model == nullptr) return;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, kModeRead) != 0 || fp == nullptr) return;

#if MMD_ORIGINAL_PROFILE
    // Validate all standard names before advancing the undo ring or applying
    // the first bone. Optional extension tails retain their ignore policy.
    char checkedLine[1024]{};
    bool namesFit = std::fgets(checkedLine, sizeof checkedLine, fp) != nullptr &&
        std::fgets(checkedLine, sizeof checkedLine, fp) != nullptr &&
        std::fgets(checkedLine, sizeof checkedLine, fp) != nullptr;
    char* suffix = namesFit ? std::strstr(checkedLine, ".osm;") : nullptr;
    if (suffix) *suffix = '\0';
    namesFit = namesFit && suffix && original_import::Legacy(checkedLine, sizeof checkedLine, 20);
    int expectedBones = -1;
    if (!std::fgets(checkedLine, sizeof checkedLine, fp) ||
        std::sscanf(checkedLine, "%d;", &expectedBones) != 1 || expectedBones < 0)
        namesFit = false;
    int foundBones = 0;
    while (namesFit && foundBones < expectedBones && std::fgets(checkedLine, sizeof checkedLine, fp)) {
        if (std::strncmp(checkedLine, "Bone", 4) != 0) continue;
        char* brace = std::strchr(checkedLine, '{');
        if (!brace) { namesFit = false; break; }
        char* end = std::strpbrk(brace + 1, "\r\n");
        if (end) *end = '\0';
        namesFit = end && original_import::Legacy(brace + 1, std::strlen(brace + 1), 20);
        ++foundBones;
    }
    if (!namesFit || foundBones != expectedBones) {
        std::fclose(fp);
        original_import::Refuse(app->Hwnd(), app->EnglishUI());
        return;
    }
    std::rewind(fp);
#endif

    char line[256], name[256], tail[256];
    std::fgets(line, 0x100, fp);   // "Vocaloid Pose Data file"
    std::fgets(line, 0x100, fp);   // blank
    std::fgets(line, 0x100, fp);   // "<model>.osm;..."
    int count = 0;
    std::fscanf(fp, kCountScan, &count);
    // Deliberate hardening: the original VPD loader (0x418A10) reads this
    // count directly from the file and passes it to operator new, which
    // multiplies it by the 36-byte snapshot record with no range check, so a
    // negative or enormous count leaves bad_alloc uncaught and terminates the
    // process. The port caps the count at the bone key pool's runtime capacity
    // and drops the whole pose when the file's count falls outside that range;
    // the two tails below are still consumed, so the stream position does not
    // change.
    // Defect B3-14.
    if (count < 0 ||
        static_cast<std::size_t>(count) > mdl::BoneKeyCapacity(*mdl::Mdl(model)))
        count = 0;
    std::fgets(tail, 0x100, fp);   // count-line comment tail
    std::fgets(tail, 0x100, fp);   // blank

#if MMD_ORIGINAL_PROFILE
    const auto& namedModel = *mdl::Mdl(model);
    std::vector<std::pair<bool, std::string>> canonicalNames(namedModel.boneCount);
    for (unsigned i = 0; i < namedModel.boneCount; ++i) {
        const auto& bone = namedModel.boneTable[i];
        canonicalNames[i].first = original_file_encoding::Name(bone.name, 20, bone.jpText, 20,
                                                              canonicalNames[i].second);
    }
#endif
    const HWND hwnd = static_cast<HWND>(app->Hwnd());
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);

    // ---- undo ring (model+0x31B4 wraps at 30; slots at model+0x26EC) ---
    mdl::ModelRecord& modelRecord = *mdl::Mdl(model);
    modelRecord.undoDirty = 1;                          // 0x418AE9
    modelRecord.redoDirty = 0;                          // 0x418AF0
    int ring = ++mdl::Mdl(model)->undoState[0];
    if (ring >= 30) mdl::Mdl(model)->undoState[0] = 0;
    ring = mdl::Mdl(model)->undoState[0];
    mdl::Mdl(model)->undoState[1] = ring;
#if !MMD_ORIGINAL_PROFILE
    bone_scale::CaptureUndo(model);
#endif

    mdl::UndoRecord& undo = mdl::Mdl(model)->undoRings[0].slots[ring];
    undo.operation = 1;
    undo.dirty = 0;
    if (undo.bonePose != nullptr) {
        free(undo.bonePose);
        undo.bonePose = nullptr;
    }
    mdl::BonePoseSnapshot* const snap =
        static_cast<mdl::BonePoseSnapshot*>(
            operator new(sizeof(mdl::BonePoseSnapshot) * modelRecord.boneCount));
    undo.bonePose = snap;
    std::memset(snap, 0, sizeof(mdl::BonePoseSnapshot) * modelRecord.boneCount);

    const int boneCnt = modelRecord.boneCount;
    std::vector<unsigned char> captured(static_cast<std::size_t>(boneCnt), 0);
    mikudancestudio::mdl::BoneRecord* const bones = mikudancestudio::mdl::Bones(model);
    unsigned char* const selBits = modelRecord.boneSelection;
    unsigned char* const regBits = modelRecord.bonePhysicsState;
    const bool pmx = modelRecord.physicsMode == 2;              // 0x418FC4
    // The bones the standard segment writes; the scale extension may only
    // land on one of them (vpd_scale_io.hpp, resolution rule 3).
#if !MMD_ORIGINAL_PROFILE
    std::vector<std::uint8_t> touched(static_cast<std::size_t>(boneCnt), 0);
#endif

    for (int k = 0; k < count; ++k) {
        float trans[3] = {0, 0, 0};
        float quat[4] = {0, 0, 0, 1.0f};

        std::fgets(line, 0x100, fp);                    // "BoneN{name"
        const char* brace = std::strstr(line, kBrace);
        if (brace == nullptr) continue;                 // original reads on
        sprintf_s(name, 0x100, "%s", brace + 1);
        if (char* nl = strchr(name, 10)) *nl = 0;  // 0x507670 strchr

        std::fscanf(fp, kTransScan, &trans[0], &trans[1], &trans[2]);
        std::fgets(tail, 0x100, fp);                    // comment tail
        std::fscanf(fp, kQuatScan, &quat[0], &quat[1], &quat[2], &quat[3]);
        std::fgets(tail, 0x100, fp);                    // "}\n"
        std::fgets(tail, 0x100, fp);                    // "\n"
        std::fgets(tail, 0x100, fp);

#if MMD_ORIGINAL_PROFILE
        // PMX keeps the complete Unicode name separately from its legacy
        // mirror, which can reserve a NUL and therefore contain only 19 of
        // the 20 bytes that the VPD writer accepts. Resolve the same exact
        // name used by SaveVpdFile first; retain the mirror as a fallback for
        // poses written by original MMD. A mirror must not shadow another
        // bone's complete name.
        bool hasCanonicalMatch = false;
        for (const auto& candidate : canonicalNames)
            if (candidate.first && candidate.second == name) { hasCanonicalMatch = true; break; }
        const auto inputNameLength = std::strlen(name);
#endif
        for (int i = 0; i < boneCnt; ++i) {
            mikudancestudio::mdl::BoneRecord* const bone = bones + i;
#if MMD_ORIGINAL_PROFILE
            if (hasCanonicalMatch ? !(canonicalNames[i].first && canonicalNames[i].second == name) :
                !(strnlen_s(bone->name, 20) == inputNameLength &&
                  std::memcmp(bone->name, name, inputNameLength) == 0))
#else
            if (std::strcmp(name, bone->name) != 0)
#endif
                continue;

            // snapshot the pre-load pose into the undo record (0x418E2D..)
            if (!captured[i]) {
                captured[i] = 1;
                auto& rec = snap[undo.dirty++];
                rec.boneIndex = i;
                std::memcpy(rec.position, bone->trans, sizeof rec.position);
                std::memcpy(rec.rotation, bone->rotQuat, sizeof rec.rotation);
                rec.physicsDisabled = regBits[i];
            }

            // twist correction - same algorithm as registrar 0x49D880
            // (0x418F5C): ((u16@&bone->flags & 0x400) && type==4) || type==8
            const mdl::BoneType btype = bone->type;
            if (btype == mdl::BoneType::FixedAxis ||
                (btype == mdl::BoneType::UnderIk &&
                 (bone->flags & mdl::kBoneFlagFixedAxis) ==
                     mdl::kBoneFlagFixedAxis)) {
                float boneAxis[3] = {0.0f, 0.0f, 0.0f};
                // Deliberate hardening: the original code reads bones[bone->tailBone]
                // with no range check on the tail index (the twist-correction
                // block at 0x418F5C), so a model with a tail bone index
                // outside the bone table reads past the end. This port skips the
                // tail-based correction when the index is out of range; a
                // well-formed model always satisfies 0 <= tailBone < boneCount.
                // Defect B3-15.
                const bool tailOkay =
                    bone->tailBone >= 0 && bone->tailBone < boneCnt;
                if (pmx) {
                    std::memcpy(boneAxis, bone->axis, 12);
                } else if (tailOkay) {
                    const mdl::BoneRecord& tailBone = bones[bone->tailBone];
                    for (int c = 0; c < 3; ++c)
                        boneAxis[c] = tailBone.position[c] - bone->position[c];
                    auto* d3 = &d3dx::Get();
                    if (d3->module != nullptr)
                        d3->vec3Normalize(boneAxis, boneAxis);
                }
                auto* d3 = &d3dx::Get();
                if ((pmx || tailOkay) && d3->quatToAxisAngle != nullptr) {
                    float axisOut[3], angle;
                    d3->quatToAxisAngle(quat, axisOut, &angle);
                    if (!_finite(quat[3])) quat[3] = 1.0f;   // w (0x4190C4)
                    float sinHalf;
                    const float ww = quat[3] * quat[3];
                    if (ww <= 1.0f) {
                        sinHalf = sqrtf(1.0f - ww);     // 0x4190FD sqrt
                    } else {
                        quat[3] = 1.0f;
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
                    const float cosT = dot / (nA * nB);
                    if (cosT < 0.0f) sinHalf = -sinHalf;    // 0x4191E2
                    quat[0] = sinHalf * axisOut[0];
                    quat[1] = sinHalf * axisOut[1];
                    quat[2] = sinHalf * axisOut[2];
                    if (d3->quatNormalize != nullptr)
                        d3->quatNormalize(quat, quat);
                }
            }

            selBits[i] = 1;                              // 0x419226
            regBits[i] = 1;
#if !MMD_ORIGINAL_PROFILE
            touched[static_cast<std::size_t>(i)] = 1;
            bone_scale::SetPose(model, i, {}); // Standard VPD has no scale channel.
#endif
            std::memcpy(bone->trans, trans, 12);        // 0x419256
            std::memcpy(bone->rotQuat, quat, 16);
            // No break: 0x419240's bone loop keeps scanning after a match,
            // so EVERY bone whose name equals the VPD bone gets the pose.
        }
    }
    std::fclose(fp);

    // ---- MMS addition: bone scale tail extension (vpd_scale_io.hpp) ------
    // The standard segment above is parsed exactly as the original and every
    // touched bone already fell back to the default identity scale, so an
    // ordinary VPD (no tail) and a refused tail both leave the pose without
    // inherited scale.  The tail is read only now, after the loader's own end
    // position, and applied entry by entry under the documented resolution
    // rules; the undo snapshot, the registration bytes and the selection
    // scope set above are not touched by any of it.
#if !MMD_ORIGINAL_PROFILE
    vpd_scale_io::Extension extension;
    const vpd_scale_io::TailLoad tailStatus =
        vpd_scale_io::LoadTail(path, count, extension);
    if (tailStatus == vpd_scale_io::TailLoad::Loaded) {
        std::vector<std::array<char, 20>> boneNames(static_cast<std::size_t>(boneCnt));
        for (int i = 0; i < boneCnt; ++i)
            std::memcpy(boneNames[static_cast<std::size_t>(i)].data(),
                        bones[i].name,
                        boneNames[static_cast<std::size_t>(i)].size());
        const std::vector<std::int32_t> targets =
            vpd_scale_io::Resolve(extension.entries, boneNames, touched);
        int applied = 0;
        for (std::size_t e = 0; e < extension.entries.size(); ++e) {
            if (targets[e] < 0) continue;
            bone_scale::SetPose(model, targets[e], extension.entries[e].scale);
            ++applied;
        }
        if (applied != static_cast<int>(extension.entries.size()))
            runtime_log::Write(
                "bone scale: VPD extension applied %d of %d entries (missing, "
                "ambiguous or out-of-scope bone skipped)",
                applied, static_cast<int>(extension.entries.size()));
    } else if (tailStatus != vpd_scale_io::TailLoad::Absent) {
        runtime_log::Write("bone scale: VPD extension %s",
                           vpd_scale_io::Describe(tailStatus));
    }
#endif
}

}  // namespace mikudancestudio
