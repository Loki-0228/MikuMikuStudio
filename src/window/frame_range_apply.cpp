#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/key_pool.hpp"
// ===========================================================================
// VA 0x0043E970 - model frame-range scale/apply
// ===========================================================================
// The dialog supplies an inclusive range [start,end] and a positive scale.
// Every selected family is rebuilt through the original registrar:
//   688 bone keys, 689 morph keys, 690 model display/IK keys.
// Keys inside the range are scaled around start; keys after end are shifted by
// the range's length delta.  Bone edits use MMD's paired type-4/type-2 undo
// transaction, while morph/display follow the original no-extra-ring path.
// Reference: IDA v9.32 disassembly/decompile 0x43E970..0x4403AE.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include <Windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/panel_controls.hpp"

namespace mikudancestudio {
namespace {

// Porting-era trace under MIKUDANCESTUDIO_STATE_DUMP_DIR (CMake option
// MIKUDANCESTUDIO_DIAG, default OFF); the OFF stub keeps the call sites
// valid and inlines away to nothing.
#ifdef MIKUDANCESTUDIO_DIAG
void TraceRange(const char* stage, int value = -1) {
    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_STATE_DUMP_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\frame_range_trace.log", directory);
    std::FILE* stream = nullptr;
    if (fopen_s(&stream, path, "ab") != 0 || stream == nullptr)
        return;
    if (value >= 0)
        std::fprintf(stream, "%s=%d\n", stage, value);
    else
        std::fprintf(stream, "%s\n", stage);
    std::fclose(stream);
}
#else
inline void TraceRange(const char*, int = -1) {}
#endif

template <typename T>
T& At(unsigned char* base, std::size_t offset) {
    return *reinterpret_cast<T*>(base + offset);
}

std::uint32_t ScaledRelativeFrame(std::uint32_t frame,
                                  std::uint32_t start,
                                  std::uint32_t end,
                                  double scale) {
    if (frame <= end) {
        return static_cast<std::uint32_t>(
            static_cast<std::int64_t>(
                static_cast<double>(frame - start) * scale));
    }
    const std::uint32_t scaledSpan = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(
            static_cast<double>(end - start) * scale));
    return frame + scaledSpan - end;
}

template <typename Key>
int TrackOwner(const Key* keys, int index, int rootCount) {
    while (index >= rootCount)
        index = static_cast<int>(keys[index].previous);
    return index;
}

void ClearBoneMarks(mdl::BoneKey* keys, std::size_t capacity) {
    for (std::size_t i = 0; i < capacity; ++i)
        keys[i].allocated = 0;
}

void ClearMorphMarks(mdl::MorphKey* keys) {
    const auto morphCapacity1 = mdl::MorphKeyCapacity(keys);
    for (int i = 0; i < morphCapacity1; ++i)
        keys[i].allocated = 0;
}

void ClearDisplayMarks(mdl::DisplayKey* keys) {
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(keys); i < capacity; ++i)
        keys[i].allocated = 0;
}

struct DisplayCopy {
    std::uint32_t frame{};
    unsigned char view{};
    std::vector<unsigned char> ik;
    std::vector<unsigned char> relations;
};

void ScaleBoneKeys(MMDApp* app, unsigned char* model, HWND hDlg,
                   std::uint32_t start, std::uint32_t end, double scale) {
    if (IsDlgButtonChecked(hDlg, panel::kScaleBoneCheckbox) != BST_CHECKED)
        return;

    mdl::BoneKey* const keys = mdl::BoneKeys(model);
    mdl::MorphKey* const morphKeys = mdl::MorphKeys(model);
    mdl::DisplayKey* const displayKeys = mdl::DisplayKeys(model);
    mdl::BoneRecord* const bones = mdl::Bones(model);
    const int boneCount = static_cast<int>(mdl::Mdl(model)->boneCount);
    // Runtime pool capacity, resolved once for both sweeps below.
    const int boneKeyCapacity = static_cast<int>(
        mdl::BoneKeyCapacity(*mdl::Mdl(model)));

    int count = 0;
    for (int i = 0; i < boneKeyCapacity; ++i) {
        mdl::BoneKey& key = keys[i];
        key.allocated = 0;
        const std::uint32_t frame = key.frame;
        if ((frame != 0 || i < boneCount) && frame >= start) {
            key.allocated = 1;
            ++count;
        }
    }
    if (count == 0)
        return;
    TraceRange("bone.count", count);

    ClearMorphMarks(morphKeys);
    ClearDisplayMarks(displayKeys);

    std::vector<int> owners; owners.reserve(count);
    std::vector<bone_scale::Curve> scaleCurves; scaleCurves.reserve(count);
    std::vector<bone_scale::Matrix> scaleCopies;
    scaleCopies.reserve(count);
    std::vector<std::array<unsigned char, 84>> copies(
        static_cast<std::size_t>(count));
    int outIndex = 0;
    for (int i = 0; i < boneKeyCapacity; ++i) {
        const mdl::BoneKey& key = keys[i];
        if (key.allocated == 0)
            continue;
        scaleCopies.push_back(bone_scale::Key(model, i));
        scaleCurves.push_back(bone_scale::KeyCurve(model, i));
        auto& out = copies[static_cast<std::size_t>(outIndex++)];
        const int owner = TrackOwner(keys, i, boneCount);
        owners.push_back(owner);
        strcpy_s(reinterpret_cast<char*>(out.data()), 30,
                 bones[owner].name);
        At<std::uint32_t>(out.data(), 32) = ScaledRelativeFrame(
            key.frame, start, end, scale);
        std::memcpy(out.data() + 36, key.rotation, sizeof(key.rotation));
        std::memcpy(out.data() + 52, key.position, sizeof(key.position));
        out[64] = key.physicsDisabled;
        std::memcpy(out.data() + 65, key.interpolation,
                    sizeof(key.interpolation));
    }

    // Deleting the marked originals creates the first type-2 snapshot.
    // The inline block turns it into type 4 and opens the paired insertion
    // snapshot before the registrar starts touching the new records.
#if !MMD_ORIGINAL_PROFILE
    bone_curve::RangeRetime authored(model, int(start), int(end), scale);
    if (!authored.Ready()) return;
#endif
    DeleteMarkedKeyframes(app);
    TraceRange("bone.deleted");
    BeginRangeScaleBoneUndo(model, app->CurrentFrame(), count);
    TraceRange("bone.undo_open");
    ResetBoneKeyCursor(model);
    const int selected = mdl::Mdl(model)->selectedBone;
    std::size_t scaleIndex=0;
    for (auto& copy : copies) {
        mdl::Mdl(model)->selectedBone = owners[scaleIndex];
        bone_scale::Registration scaleRegistration(scaleCopies[scaleIndex], scaleCurves[scaleIndex]);
        ++scaleIndex;
        if (!RegisterBoneKey(model, copy.data(), static_cast<int>(start), 1)) {
            mdl::Mdl(model)->selectedBone = selected;
            return;
        }
    }
    mdl::Mdl(model)->selectedBone = selected;
#if !MMD_ORIGINAL_PROFILE
    authored.Complete();
#endif
    TraceRange("bone.inserted");
}

void ScaleMorphKeys(MMDApp* app, unsigned char* model, HWND hDlg,
                    std::uint32_t start, std::uint32_t end, double scale) {
    if (IsDlgButtonChecked(hDlg, panel::kScaleMorphCheckbox) != BST_CHECKED)
        return;

    mdl::BoneKey* const boneKeys = mdl::BoneKeys(model);
    mdl::MorphKey* const keys = mdl::MorphKeys(model);
    mdl::DisplayKey* const displayKeys = mdl::DisplayKeys(model);
    mdl::MorphRecord* const morphs = mdl::Morphs(model);
    const int morphCount = static_cast<int>(mdl::Mdl(model)->morphCount);

    int count = 0;
    const auto morphCapacity2 = mdl::MorphKeyCapacity(keys);
    for (int i = 0; i < morphCapacity2; ++i) {
        mdl::MorphKey& key = keys[i];
        key.allocated = 0;
        const std::uint32_t frame = key.frame;
        if ((frame != 0 || i < morphCount) && frame >= start) {
            key.allocated = 1;
            ++count;
        }
    }
    if (count == 0)
        return;
    TraceRange("morph.count", count);

    ClearBoneMarks(boneKeys, mdl::BoneKeyCapacity(*mdl::Mdl(model)));
    ClearDisplayMarks(displayKeys);

    std::vector<int> owners; owners.reserve(count);
    std::vector<std::array<unsigned char, 40>> copies(
        static_cast<std::size_t>(count));
    int outIndex = 0;
    const auto morphCapacity3 = mdl::MorphKeyCapacity(keys);
    for (int i = 0; i < morphCapacity3; ++i) {
        const mdl::MorphKey& key = keys[i];
        if (key.allocated == 0)
            continue;
        auto& out = copies[static_cast<std::size_t>(outIndex++)];
        const int owner = TrackOwner(keys, i, morphCount);
        owners.push_back(owner);
        strcpy_s(reinterpret_cast<char*>(out.data()), 30,
                 morphs[owner].name);
        At<std::uint32_t>(out.data(), 32) = ScaledRelativeFrame(
            key.frame, start, end, scale);
        std::memcpy(out.data() + 36, &key.value, sizeof(key.value));
    }

    DeleteMarkedKeyframes(app);
    TraceRange("morph.deleted");
    ResetMorphKeyCursor(model);
    std::size_t copyIndex = 0;
    for (auto& copy : copies) {
        std::uint32_t frame = 0;
        if (!frame_limits::Add(start, At<std::uint32_t>(copy.data(), 32), frame) ||
            !RegisterMorphKeyAtIndex(model, owners[copyIndex++], frame, At<float>(copy.data(), 36))) break;
    }
    TraceRange("morph.inserted");
}

void ScaleDisplayKeys(MMDApp* app, unsigned char* model, HWND hDlg,
                      std::uint32_t start, std::uint32_t end, double scale) {
    if (IsDlgButtonChecked(hDlg, panel::kScaleDispIkCheckbox) != BST_CHECKED)
        return;

    mdl::BoneKey* const boneKeys = mdl::BoneKeys(model);
    mdl::MorphKey* const morphKeys = mdl::MorphKeys(model);
    mdl::DisplayKey* const keys = mdl::DisplayKeys(model);
    mdl::BoneRecord* const bones = mdl::Bones(model);
    mdl::IkChain* const ikChains = mdl::IkChains(model);
    const mdl::BoneOrderEntry* const selectors = mdl::BoneOrder(model);
    const int ikCount = static_cast<int>(mdl::Mdl(model)->ikChainCount);
    const int relationCount =
        static_cast<int>(mdl::Mdl(model)->boneOrderCount);

    int count = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(keys); i < capacity; ++i) {
        mdl::DisplayKey& key = keys[i];
        key.allocated = 0;
        const std::uint32_t frame = key.frame;
        if (frame >= start && frame != 0) {
            key.allocated = 1;
            ++count;
        }
    }
    if (count == 0)
        return;
    TraceRange("display.count", count);

    ClearBoneMarks(boneKeys, mdl::BoneKeyCapacity(*mdl::Mdl(model)));
    ClearMorphMarks(morphKeys);

    std::vector<DisplayCopy> copies;
    copies.reserve(static_cast<std::size_t>(count));
    for (std::size_t keyIndex = 0, capacity = mdl::KeyPoolCapacity(keys); keyIndex < capacity; ++keyIndex) {
        const mdl::DisplayKey& key = keys[keyIndex];
        if (key.allocated == 0)
            continue;

        DisplayCopy copy;
        copy.frame = ScaledRelativeFrame(key.frame, start, end, scale);
        copy.view = key.visible;
        copy.ik.resize(static_cast<std::size_t>(ikCount) * 21);
        const unsigned char* const keyIk = mdl::IkStates(key);
        for (int i = 0; i < ikCount; ++i) {
            unsigned char* const out = copy.ik.data() + 21 * i;
            const int bone = ikChains[i].boneIndex;
            strcpy_s(reinterpret_cast<char*>(out), 20,
                     bones[bone].name);
            out[20] = keyIk[i];
        }

        copy.relations.resize(static_cast<std::size_t>(relationCount) * 28);
        const mdl::BoneReference* const keyRelations =
            mdl::SelectorStates(key);
        for (int i = 0; i < relationCount; ++i) {
            unsigned char* const out = copy.relations.data() + 28 * i;
            if (i != 0) {
                const int bone = selectors[i].boneIndex;
                strcpy_s(reinterpret_cast<char*>(out), 20,
                         bones[bone].name);
            }
            At<std::int32_t>(out, 20) = keyRelations[i].modelIndex;
            At<std::int32_t>(out, 24) = keyRelations[i].boneIndex;
        }
        copies.push_back(std::move(copy));
    }

    DeleteMarkedKeyframes(app);
    TraceRange("display.deleted");
    ResetDisplayKeyCursor(model);
    for (const auto& copy : copies) {
        if (!RegisterDisplayKeyFromRecord(model, static_cast<int>(copy.frame), copy.view,
                       ikCount, copy.ik.data(), relationCount,
                       copy.relations.data(), static_cast<int>(start)))
            break;
    }
    TraceRange("display.inserted");
}

}  // namespace

// was Sub43E970, VA 0x0043E970 - read the dialog's start/end/scale fields
// and re-register the selected key families through the scaled range.
void ApplyFrameRangeScale(HWND hDlg) {
    TraceRange("entered");
    MMDApp* const app = g_Block;
    if (app == nullptr)
        return;
    unsigned char* const model = app->SelectedModel();
    if (model == nullptr)
        return;

    char text[256]{};
    GetWindowTextA(GetDlgItem(hDlg, panel::kScaleFromEdit), text, sizeof text);
    const std::int32_t startSigned = std::atol(text);
    GetWindowTextA(GetDlgItem(hDlg, panel::kScaleToEdit), text, sizeof text);
    const std::int32_t endSigned = std::atol(text);
    TraceRange("start", startSigned);
    TraceRange("end", endSigned);
    if (startSigned<0||endSigned<=startSigned||endSigned>frame_limits::maximum)
        return;

    // (between modelOffsetZ and morphFrameShift), no state member yet.

    GetWindowTextA(GetDlgItem(hDlg, panel::kScaleRateEdit), text, 8);
    const double scale = std::atof(text);
    TraceRange("scale");
    if (!std::isfinite(scale)||scale < 0.0000099999997 || scale == 1.0)
        return;

    const std::uint32_t start = static_cast<std::uint32_t>(startSigned);
    const std::uint32_t end = static_cast<std::uint32_t>(endSigned);
    const auto valid=[&](const auto* keys,std::size_t capacity){
        for(std::size_t i=0;keys&&i<capacity;++i){const auto frame=keys[i].frame;if(frame<start)continue;
            const double mapped=frame<=end?double(start)+(double(frame)-start)*scale:double(frame)+(double(end)-start)*(scale-1.0);
            if(!std::isfinite(mapped)||mapped<0||mapped>frame_limits::maximum)return false;
        }return true;
    };
    if((IsDlgButtonChecked(hDlg,panel::kScaleBoneCheckbox)==BST_CHECKED&&!valid(mdl::BoneKeys(model),mdl::BoneKeyCapacity(*mdl::Mdl(model))))||
       (IsDlgButtonChecked(hDlg,panel::kScaleMorphCheckbox)==BST_CHECKED&&!valid(mdl::MorphKeys(model),mdl::MorphKeyCapacity(mdl::MorphKeys(model))))||
       (IsDlgButtonChecked(hDlg,panel::kScaleDispIkCheckbox)==BST_CHECKED&&!valid(mdl::DisplayKeys(model),mdl::KeyPoolCapacity(mdl::DisplayKeys(model))))){
        const wchar_t* messages[]={L"変換後のフレーム番号が保存可能な範囲を超えます。変更は適用していません。",L"The resulting frame number cannot be represented. No edits were applied.",L"转换后的帧号超出可表示范围，未应用修改。"};
        MessageBoxW(hDlg,messages[app->EnglishUI()<=2?app->EnglishUI():1],L"MMS",MB_OK|MB_ICONWARNING);return;
    }
    app->FrameRangeStartFrame()=startSigned;
    TraceRange("begin");
    ScaleBoneKeys(app, model, hDlg, start, end, scale);
    ScaleMorphKeys(app, model, hDlg, start, end, scale);
    ScaleDisplayKeys(app, model, hDlg, start, end, scale);
    TraceRange("families_done");

    const std::uint32_t modelMax = mdl::Mdl(model)->maxFrame;
    if (app->LastRegisteredFrame() < modelMax)
        app->LastRegisteredFrame() = modelMax;
    SeekModelFrame(model, app->CurrentFrame(),
              app->PlaybackPhysicsMode());
    PanelPaint(app);
    SelectionReeval(app);
    app->SceneModified() = 1;
    TraceRange("done");
}

}  // namespace mikudancestudio
