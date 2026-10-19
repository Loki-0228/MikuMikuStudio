#include "mikumikustudio/original_import.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/frame_rate_correction.hpp"
#include "mikumikustudio/sequence_scatter.hpp"
#include "viewport_corner_menu.hpp"
// ===========================================================================
// Enhance-model IO: toon collect (0x41EA20), save model (0x41EC10), model colour (0x4A4850)
// ===========================================================================
// Split out of src/window/command_view_menu.cpp (the menu-251..302 command
// family) so the dialog's helper bodies can be ported independently.
// Every function keeps its original x86 VA; behaviour notes live in the
// per-function comments.  Ported against the x64 twin
// (MikuMikuDanceE_v932x64, session 99824acb):
//   0x41EA20 -> sub_7FF7CB4AD5D0   0x41EC10 -> sub_7FF7CB4D59B0
//   0x4A4850 -> sub_7FF7CB4F2240   0x40B5A0 -> sub_7FF7CB43AB40
//   0x4076E0 -> sub_7FF7CB428F30
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/skinning_menu.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/model_alpha_pass.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/view_options.hpp"
#include "mikumikustudio/ui_language.hpp"

#include "mikumikustudio/texture_lookup.hpp"
#if MMD_ORIGINAL_PROFILE
#include "mikumikustudio/original_file_encoding.hpp"
#include <array>
#include <set>
#include <vector>
#endif

namespace mikumikustudio {
namespace {

// ---- narrow/wide name helpers of the enhance-model chain ------------------
// PMD legacy filenames are Shift-JIS regardless of the active Windows code page.

// x64 sub_7FF7CB429290 - convert a wide name to SJIS (CP 932); the output is
// left untouched when the input is empty.
void WideToSjis(D3DRenderer* renderer, char* out, const wchar_t* wide,
                std::size_t outBytes) {
    if (*wide == L'\0')
        return;
    (void)renderer;
    std::string encoded;
    if (text_encoding::EncodeExact(wide, 932, encoded) && encoded.size() < outBytes)
        memcpy(out, encoded.c_str(), encoded.size() + 1);
}

constexpr UINT kD3dxDefault = 0xFFFFFFFFu;

enum class PmdRefusal { Representation, Write };

void RefusePmd(MMDApp* app, PmdRefusal reason) {
    const wchar_t* incompatible[] = {
        L"このモデルの名前、材質参照、または構造を標準 PMD に完全に保存できません。PMX から PMD への変換には対応していません。既存のファイルは変更していません。",
        L"The model names, material references or structure cannot be preserved in standard PMD. PMX-to-PMD conversion is unsupported. The existing file was not changed.",
        L"模型的名称、材质引用或结构无法完整保存为标准 PMD；此版本不支持将 PMX 转换为 PMD。已有文件未改动。"};
    const wchar_t* failed[] = {
        L"PMD ファイルを完全に保存できませんでした。保存先と空き容量を確認してください。既存のファイルは変更していません。",
        L"The PMD file could not be saved completely. Check the destination and free space. The existing file was not changed.",
        L"PMD 文件未能完整保存，请检查保存位置和剩余空间。已有文件未改动。"};
    runtime_log::Write("PMD save refused: %s",
        reason == PmdRefusal::Representation ? "not representable" : "write failed");
    if (app && app->Hwnd()) {
        const unsigned language = app->EnglishUI() <= 2 ? app->EnglishUI() : 1;
        MessageBoxW(static_cast<HWND>(app->Hwnd()),
            (reason == PmdRefusal::Representation ? incompatible : failed)[language],
            L"PMD", MB_OK | MB_ICONERROR);
    }
    SetLastError(reason == PmdRefusal::Representation ? ERROR_INVALID_DATA : ERROR_WRITE_FAULT);
}

// The shared writer emits the narrow fields verbatim. Original MMD consumes
// these as C strings (and forces the final English byte to zero), so reserve a
// terminator and validate complete CP932 identities against authoritative text.
#if MMD_ORIGINAL_PROFILE
bool OriginalPmdName(const char* raw, std::size_t capacity, const wchar_t* wide = nullptr) {
    std::string encoded;
    return capacity && original_file_encoding::Name(raw, capacity, wide, capacity - 1, encoded) &&
        encoded == std::string(raw, strnlen_s(raw, capacity));
}

bool PrepareOriginalPmd(mdl::ModelRecord& model,
                        std::vector<std::array<char, 20>>& textures) {
    if (model.physicsMode == 2 || model.pmxVertices ||
        model.boneCount == 0 || model.boneCount > 65535 ||
        model.morphCount > 65535 || model.ikChainCount > 65535 ||
        !model.boneTable || (model.vertexCount && !model.rawVertices) ||
        (model.indexCount && !model.indices) || (model.materialCount && !model.materials) ||
        (model.ikChainCount && !model.ikChains) || (model.morphCount && !model.morphs) ||
        (model.morph0Count && !model.morph0Table) ||
        (model.facialFrameCount && !model.displayFrames) ||
        (model.rigidBodyCount && !model.rbGroups) ||
        (model.rigidCount && !model.rigidTable) || (model.jointCount && !model.jointTable)) return false;
    if (!OriginalPmdName(model.name, sizeof model.name, model.pmxTextBuffers[0]) ||
        !OriginalPmdName(model.nameEn, sizeof model.nameEn, model.pmxTextBuffers[1]) ||
        !OriginalPmdName(model.comment, sizeof model.comment, model.pmxTextBuffers[2]) ||
        !OriginalPmdName(model.commentEn, sizeof model.commentEn, model.pmxTextBuffers[3])) return false;
    const auto signedBoneIndex = [&](int index, bool allowNone = false) {
        return (allowNone && index == -1) ||
            (index >= 0 && index <= 32767 && static_cast<unsigned>(index) < model.boneCount);
    };
    // IK and display references are zero-extended WORDs in the PMD loader,
    // unlike parent/tail, vertex skinning and rigid-body bone references.
    const auto unsignedBoneIndex = [&](int index) {
        return index >= 0 && index <= 65535 && static_cast<unsigned>(index) < model.boneCount;
    };
    for (unsigned i = 0; i < model.boneCount; ++i) {
        const auto& bone = model.boneTable[i];
        if (!OriginalPmdName(bone.name, sizeof bone.name, bone.jpText) ||
            !OriginalPmdName(bone.nameEn, sizeof bone.nameEn, bone.enText) ||
            !signedBoneIndex(bone.parent, true) || !signedBoneIndex(bone.tailBone, true) ||
            bone.tailIdx < -32768 || bone.tailIdx > 32767 ||
            static_cast<unsigned>(bone.type) > 9) return false;
        // CoRotate uses tailIdx as a percentage, not a bone reference.
        if ((bone.type == mdl::BoneType::UnderIk || bone.type == mdl::BoneType::RotateGrant) &&
            !signedBoneIndex(bone.tailIdx)) return false;
    }
    for (unsigned i = 0; i < model.vertexCount; ++i) {
        const auto& vertex = model.rawVertices[i];
        if (vertex.weightPercent < 0 || vertex.weightPercent > 100 ||
            !signedBoneIndex(vertex.bone[0], vertex.weightPercent == 0) ||
            !signedBoneIndex(vertex.bone[1], vertex.weightPercent == 100)) return false;
    }
    const auto* indices = mdl::Indices(reinterpret_cast<unsigned char*>(&model));
    if (model.indexCount % 3) return false;
    for (unsigned i = 0; i < model.indexCount; ++i)
        if (indices[2 * static_cast<std::size_t>(i)] >= model.vertexCount) return false;
    for (unsigned i = 0; i < model.ikChainCount; ++i) {
        const auto& ik = model.ikChains[i];
        if (!unsignedBoneIndex(ik.boneIndex) || !unsignedBoneIndex(ik.targetBone) || (ik.linkCount && !ik.links)) return false;
        for (unsigned j = 0; j < ik.linkCount; ++j) if (!unsignedBoneIndex(ik.links[j])) return false;
    }
    std::set<std::uint32_t> baseVertices;
    for (unsigned i = 0; i < model.morphCount; ++i) {
        const auto& morph = model.morphs[i];
        if (!OriginalPmdName(morph.name, sizeof morph.name, morph.jpText) ||
            (i && !OriginalPmdName(morph.nameEn, sizeof morph.nameEn, morph.enText)) ||
            morph.offsetCount < 0 || (morph.offsetCount && !morph.vertexEntries) ||
            static_cast<unsigned>(morph.panel) > 4) return false;
        for (int j = 0; j < morph.offsetCount; ++j) {
            const auto vertex = morph.vertexEntries[j].vertexIndex;
            if (vertex >= model.vertexCount) return false;
            if (i == 0) { if (!baseVertices.insert(vertex).second) return false; }
            else if (!baseVertices.count(vertex)) return false;
        }
    }
    for (unsigned i = 0; i < model.morph0Count; ++i)
        if (model.morph0Table[i].vertexIndex >= model.vertexCount) return false;
    for (unsigned i = 0; i < model.facialFrameCount; ++i)
        if (model.displayFrames[i].targetIndex >= model.morphCount) return false;
    const unsigned fixedGroups = model.facialFrameCount ? 3 : 2;
    if (model.groupCount < fixedGroups || (model.groupCount > fixedGroups && !model.groupNames)) return false;
    auto* groups = mdl::DisplayGroups(reinterpret_cast<unsigned char*>(&model));
    for (unsigned i = fixedGroups; i < model.groupCount; ++i)
        if (!OriginalPmdName(groups[i].name, sizeof groups[i].name) ||
            !OriginalPmdName(groups[i].nameEn, sizeof groups[i].nameEn)) return false;
    auto* rows = mdl::RigidGroups(reinterpret_cast<unsigned char*>(&model));
    for (unsigned i = 0; i < model.rigidBodyCount; ++i)
        if (!unsignedBoneIndex(rows[i].targetIndex) || rows[i].groupIndex < fixedGroups ||
            rows[i].groupIndex >= model.groupCount) return false;
    for (const auto& toon : model.pmdToonFileNames)
        if (!OriginalPmdName(toon, sizeof toon)) return false;
    for (unsigned i = 0; i < model.rigidCount; ++i) {
        const auto& rigid = model.rigidTable[i];
        if (!OriginalPmdName(rigid.name, sizeof rigid.name, rigid.jpText) ||
            !signedBoneIndex(rigid.boneIndex, true) || rigid.group > 15 ||
            rigid.shape > 2 || rigid.mode > 2) return false;
    }
    for (unsigned i = 0; i < model.jointCount; ++i) {
        const auto& joint = model.jointTable[i];
        if (!OriginalPmdName(joint.name, sizeof joint.name, joint.jpText) ||
            joint.rigidA < 0 || joint.rigidB < 0 ||
            static_cast<unsigned>(joint.rigidA) >= model.rigidCount ||
            static_cast<unsigned>(joint.rigidB) >= model.rigidCount) return false;
    }
    const std::size_t directoryLength = wcsnlen_s(model.modelDirectory, 256);
    if (directoryLength == 256) return false;
    const auto relativePath = [&](const wchar_t* path, std::wstring& relative) {
        const std::size_t length = wcsnlen_s(path, 280);
        if (length == 280) return false;
        if (!length) { relative.clear(); return true; }
        if (length < directoryLength || _wcsnicmp(path, model.modelDirectory, directoryLength)) return false;
        relative.assign(path + directoryLength, length - directoryLength);
        return relative.find_first_of(L":*") == std::wstring::npos &&
            (relative.empty() || (relative[0] != L'\\' && relative[0] != L'/'));
    };
    auto* materials = mdl::Materials(reinterpret_cast<unsigned char*>(&model));
    std::uint64_t materialIndices = 0;
    textures.resize(model.materialCount);
    for (unsigned i = 0; i < model.materialCount; ++i) {
        const auto& material = materials[i];
        std::wstring texture, sphere;
        std::string encoded;
        if (material.faceVertexCount < 0 || material.faceVertexCount % 3 ||
            (material.toonReference > 9 && material.toonReference != 255) ||
            !relativePath(material.texturePath, texture) || !relativePath(material.spherePath, sphere) ||
            !original_file_encoding::Encode((texture + (sphere.empty() ? L"" : L"*" + sphere)).c_str(), 20, encoded)) return false;
        materialIndices += static_cast<unsigned>(material.faceVertexCount);
        std::memcpy(textures[i].data(), encoded.data(), encoded.size());
    }
    return materialIndices == model.indexCount;
}
#endif

}  // namespace

bool original_import::CanEdit(mdl::ModelRecord& model) {
#if MMD_ORIGINAL_PROFILE
    std::vector<std::array<char, 20>> textures;
    return mdl::DisplayGroupCount(model) <= 255 && mdl::FacialFrameCount(model) <= 255 &&
        PrepareOriginalPmd(model, textures);
#else
    return true;
#endif
}

bool original_import::Model(mdl::ModelRecord& model) {
#if MMD_ORIGINAL_PROFILE
    if (!ModelNames(model)) return false;
    if (model.physicsMode != 2) return CanEdit(model);
    // PMX remains available for scenes, but its full names must fit the
    // standard PMM/VMD identities. It is never converted by the PMD editor.
    for (unsigned i = 0; i < model.materialCount; ++i) {
        const auto& material = mdl::Materials(reinterpret_cast<unsigned char*>(&model))[i];
        if (!Text(material.texturePath) || !Text(material.spherePath) ||
            !Text(text_encoding::MaterialName(model, i, false).c_str()) ||
            !Text(text_encoding::MaterialName(model, i, true).c_str())) return false;
    }
    const auto* groups = static_cast<const mdl::DisplayGroup*>(model.groupNames);
    for (unsigned i = 0; groups && i < mdl::DisplayGroupCount(model); ++i)
        if (!Text(text_encoding::DisplayGroupName(groups[i], false).c_str(), 49) ||
            !Text(text_encoding::DisplayGroupName(groups[i], true).c_str(), 49)) return false;
    for (unsigned i = 0; i < model.rigidCount; ++i)
        if (!Text(model.rigidTable[i].jpText, 19) || !Text(model.rigidTable[i].enText)) return false;
    for (unsigned i = 0; i < model.jointCount; ++i)
        if (!Text(model.jointTable[i].jpText, 19) || !Text(model.jointTable[i].enText)) return false;
#endif
    return true;
}

// ===========================================================================
// 0x41EA20 (x64 sub_7FF7CB4AD5D0) - OK collector of the enhance-model toon
// dialog (menu 261, edits 709..718).  For each of the ten slots:
//   * a name equal to the default "toonNN.bmp" is accepted as-is (skip);
//   * otherwise the name is converted to wide and probed as
//     <model directory>\<name> through the shared texture cache
//     (LoadTextureShared, 0x407490); on success the SJIS name is stored
//     into the model's pmdToonFileNames slot;
//   * on failure the EN/JP "Cannot find ..." box appears and the function
//     will report failure.
// Returns 1 when every non-default name resolved.
// ===========================================================================
int CollectToonFileNames(HWND hDlg) {  // was Sub41EA20, VA 0x0041EA20
    MMDApp* app = g_Block;
    mdl::ModelRecord& model = *mdl::Mdl(app->SelectedModel());
    D3DRenderer* renderer = app->Renderer();
    int allFound = 1;
    for (int i = 1; i <= 10; ++i) {
        wchar_t wideName[0x100]{};
        GetWindowTextW(GetDlgItem(hDlg, 708 + i), wideName, 0x100);
        std::string encoded;
        const bool representable =
            text_encoding::EncodeExact(wideName, 932, encoded) &&
            encoded.size() < sizeof(model.pmdToonFileNames[i - 1]);
        char name[0x100]{};
        if (representable)
            memcpy(name, encoded.c_str(), encoded.size() + 1);
        char def[0x100];
        sprintf_s(def, 0x100, "toon%02d.bmp", i);
        if (!representable) {
            MessageBoxW(hDlg,
                        L"文件名必须能用日文 Shift-JIS（CP932）编码，且长度不能超过 99 字节。",
                        L"Toon 纹理", MB_OK | MB_ICONWARNING);
            allFound = 0;
            continue;
        }
        if (std::strcmp(name, def) == 0)
            continue;  // default toon name: no file probe, no store
        wchar_t rel[0x100]{};
        wcscpy_s(rel, 0x100, wideName);
        wchar_t path[0x100];
        swprintf_s(path, 0x100, L"%s%s", model.modelDirectory, rel);
        if (path[0] != L'\0' &&
            LoadTextureShared(reinterpret_cast<unsigned char*>(renderer),
                              path) != 0) {
            strcpy_s(model.pmdToonFileNames[i - 1], 0x64, name);
        } else {
            allFound = 0;
            char text[0x100];
            const char* caption;
            if (app->state.englishUI != 0) {
                sprintf_s(text, 0x100,
                          "Cannot find '%s'.\n\n"
                          "Please put '%s' in the same folder as model file "
                          "(*.pmd).",
                          name, name);
                caption = "toon texture";
            } else {
                // 0x52F0F0: "ﾂｰﾝﾃｷｽﾁｬｱ:%sが見つかりません。\n\n
                //            %sをモデルファイル(*.pmd)と同じフォルダ内に
                //            置いてください。"
                sprintf_s(text, 0x100,
                          "\xc3\xb8\xbd\xc1\xac\xcc\xa7\xb2\xd9\x81\x46%s"
                          "\x82\xaa\x8c\xa9\x82\xc2\x82\xa9\x82\xe8\x82\xdc"
                          "\x82\xb9\x82\xf1\x81\x42\n\n"
                          "%s\x82\xf0\xd3\xc3\xde\xd9\xcc\xa7\xb2\xd9"
                          "(*.pmd)\x82\xc6\x93\xaf\x82\xb6\x83\x74\x83\x48"
                          "\x83\x8b\x83\x5f\x93\xe0\x82\xc9\x92\x75\x82\xa2"
                          "\x82\xc4\x89\xba\x82\xb3\x82\xa2\x81\x42",
                          name, name);
                // 0x52F148: "トゥーンテクスチャ"
                caption = "\x83\x67\x83\x44\x81\x5B\x83\x93\x83\x65\x83\x4E"
                          "\x83\x58\x83\x60\x83\x83";
            }
            LegacyUiMessageBoxA(hDlg, text, caption, 0);
        }
    }
    return allFound;
}

// ===========================================================================
// 0x41EC10 (x64 sub_7FF7CB4D59B0) - "save enhanced model" writer.  The x64
// original is a method of the model (thiscall(model, path, renderer)); the
// dispatcher passes the selected model, so pick it from the app here.
// Serialises the whole model as a PMD file (field order mirrors the reader
// in src/model/pmd_load.cpp):
//   header ("Pmd", 1.0f, name[20], comment[256]) - vertex count + 38-byte
//   records (edge flag normalised to 0/1) - index count + u16 indices -
//   material count + 70-byte records (texture+sphere names combined with
//   '*' after stripping the model-directory prefix, converted to SJIS) -
//   bone count + 39-byte records - IK count + chains - morph count +
//   entries (indices of morph 0 written verbatim, later morphs remapped
//   through morph 0's table) - facial display list - bone group names -
//   rigid-body display list - English section (flag always 1; morph names
//   skip morph 0, group names start at 2/3 like the reader) - the ten
//   100-byte toon file names - rigid bodies (83 bytes) - joints (124
//   bytes, limits in the reader's shuffled order).  On success the saved
//   path becomes model->path.
// Quirk kept verbatim: before writing vertices, every morph-0 entry
// OVERWRITES (not adds to) the raw vertex position with its offset.
// ===========================================================================
void SaveEnhancedModel(MMDApp* app, const wchar_t* path) {  // was Sub41EC10, 0x41EC10
    if (!app || !app->SelectedModel() || !path || !*path || wcsnlen_s(path, 256) == 256) {
        RefusePmd(app, PmdRefusal::Representation);
        return;
    }
    mdl::ModelRecord& model = *mdl::Mdl(app->SelectedModel());
#if MMD_ORIGINAL_PROFILE
    if (mdl::DisplayGroupCount(model)>255 || mdl::FacialFrameCount(model)>255) {
        RefusePmd(app,PmdRefusal::Representation);return;
    }
#else
    if (!save_compatibility::CanSavePmd(model)) {
        const wchar_t* message = L"PMD 无法无损保存这些名称或显示项，操作已取消。请保留原模型并保存 PMM 工程。\nPMD cannot preserve these names or display entries. Keep the source model and save a PMM project.";
        if(app->MainWindow())MessageBoxW(app->MainWindow(), message, L"PMD", MB_OK | MB_ICONWARNING);
        SetLastError(ERROR_INVALID_DATA);return;
    }
#endif
    D3DRenderer* renderer = app->Renderer();

    int fh = -1;
#if MMD_ORIGINAL_PROFILE
    std::vector<std::array<char, 20>> textureNames;
    if (!PrepareOriginalPmd(model, textureNames)) {
        RefusePmd(app, PmdRefusal::Representation);
        return;
    }
#endif
    // Write beside the destination, then publish only a complete PMD. A bad
    // name, short write, or failed replacement must leave the old file intact.
    wchar_t destination[MAX_PATH]{};
    wchar_t* filename = nullptr;
    const DWORD destinationLength = GetFullPathNameW(path, MAX_PATH, destination, &filename);
    if (!destinationLength || destinationLength >= MAX_PATH || !filename || !*filename) {
        RefusePmd(app, PmdRefusal::Write);
        return;
    }
    wchar_t directory[MAX_PATH]{};
    wcsncpy_s(directory, destination, static_cast<std::size_t>(filename - destination));
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(directory, L"mmd", 0, temporary)) {
        RefusePmd(app, PmdRefusal::Write);
        return;
    }
    if (_wsopen_s(&fh, temporary, _O_BINARY | _O_WRONLY | _O_TRUNC,
                  _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0 || fh == -1) {
        DeleteFileW(temporary);
        RefusePmd(app, PmdRefusal::Write);
        return;
    }
    bool writeFailed = false;
    const auto _write = [&](int descriptor, const void* bytes, unsigned count) {
        if (writeFailed) return -1;
        const int written = ::_write(descriptor, bytes, count);
        if (written < 0 || static_cast<unsigned>(written) != count) writeFailed = true;
        return written;
    };

    // ---- header ----------------------------------------------------------
    _write(fh, "Pmd", 3);
    const float version = 1.0f;
    _write(fh, &version, 4);
    _write(fh, model.name, 0x14);
    _write(fh, model.comment, 0x100);

    // ---- morph-0 pre-pass (positions overwritten, see note above) --------
    model_vertex_cache::Invalidate(reinterpret_cast<unsigned char*>(&model));
    if (model.morphs != nullptr && model.morph0Count != 0) {
        for (std::uint32_t k = 0; k < model.morph0Count; ++k) {
            const mdl::PmdVertexMorphEntry& e = model.morph0Table[k];
            std::memcpy(model.rawVertices[e.vertexIndex].position, e.offset,
                        12);
        }
    }

    // ---- vertices --------------------------------------------------------
    _write(fh, &model.vertexCount, 4);
    for (std::uint32_t i = 0; i < model.vertexCount; ++i) {
        const mdl::PmdVertex& v = model.rawVertices[i];
        _write(fh, v.position, 12);
        _write(fh, v.normal, 12);
        _write(fh, v.uv, 8);
        _write(fh, &v.bone[0], 2);
        _write(fh, &v.bone[1], 2);
        _write(fh, &v.weightPercent, 1);
        const std::uint8_t edge = v.edgeDisabled != 0 ? 1 : 0;
        _write(fh, &edge, 1);
    }

    // ---- indices ---------------------------------------------------------
    _write(fh, &model.indexCount, 4);
    std::uint16_t* const indices =
        mdl::Indices(reinterpret_cast<unsigned char*>(&model));
    // Both native loaders keep four-byte CPU index slots. PMD fills their
    // low WORDs and PMX normalizes to int32; the PMD file uses packed WORDs.
    for (std::uint32_t i = 0; i < model.indexCount; ++i)
        _write(fh, &indices[2 * static_cast<std::size_t>(i)], 2);

    // ---- materials -------------------------------------------------------
    _write(fh, &model.materialCount, 4);
    mdl::ModelMaterialRecord* const materials = mdl::Materials(
        reinterpret_cast<unsigned char*>(&model));
    const std::size_t dirLen = std::wcslen(model.modelDirectory);
    for (std::uint32_t i = 0; i < model.materialCount; ++i) {
        const mdl::ModelMaterialRecord& mat = materials[i];
        _write(fh, mat.diffuse, 16);
        _write(fh, &mat.specularPower, 4);
        _write(fh, mat.specular, 12);
        _write(fh, mat.ambient, 12);
        _write(fh, &mat.toonReference, 1);
        const std::uint8_t sphereFlag = mat.doubleSided != 0 ? 1 : 0;
        _write(fh, &sphereFlag, 1);
        _write(fh, &mat.faceVertexCount, 4);
#if MMD_ORIGINAL_PROFILE
        _write(fh, textureNames[i].data(), 20);
#else
        // combined texture[*sphere] name, stripped of the directory prefix
        wchar_t combined[20];
        const wchar_t* name;
        if (mat.spherePath[0] != L'\0') {
            wcscpy_s(combined, 0x14, mat.texturePath + dirLen);
            wcscat_s(combined, 0x14, L"*");
            wcscat_s(combined, 0x14, mat.spherePath + dirLen);
            name = combined;
        } else {
            name = mat.texturePath[0] != L'\0' ? mat.texturePath + dirLen
                                               : mat.texturePath;
        }
        char sjis[0x100] = "";
        WideToSjis(renderer, sjis, name, 0x100);
        _write(fh, sjis, 0x14);
#endif
    }

    // ---- bones -----------------------------------------------------------
    _write(fh, &model.boneCount, 2);
    for (std::uint32_t i = 0; i < model.boneCount; ++i) {
        const mdl::BoneRecord& bone = model.boneTable[i];
        _write(fh, bone.name, 0x14);
        const std::uint16_t parent =
            static_cast<std::uint16_t>(bone.parent);
        _write(fh, &parent, 2);
        const std::uint16_t tail =
            static_cast<std::uint16_t>(bone.tailBone);
        _write(fh, &tail, 2);
        _write(fh, &bone.type, 1);
        const std::uint16_t tailIdx =
            static_cast<std::uint16_t>(bone.tailIdx);
        _write(fh, &tailIdx, 2);
        _write(fh, bone.position, 12);
    }

    // ---- IK chains -------------------------------------------------------
    _write(fh, &model.ikChainCount, 2);
    for (std::uint32_t i = 0; i < model.ikChainCount; ++i) {
        const mdl::IkChain& ik = model.ikChains[i];
        const std::uint16_t bone =
            static_cast<std::uint16_t>(ik.boneIndex);
        _write(fh, &bone, 2);
        const std::uint16_t target =
            static_cast<std::uint16_t>(ik.targetBone);
        _write(fh, &target, 2);
        _write(fh, &ik.linkCount, 1);
        _write(fh, &ik.iterations, 2);
        _write(fh, &ik.maxAngle, 4);
        for (int k = 0; k < ik.linkCount; ++k)
            _write(fh, &ik.links[k], 2);
    }

    // ---- morphs ----------------------------------------------------------
    _write(fh, &model.morphCount, 2);
    for (std::uint32_t i = 0; i < model.morphCount; ++i) {
        const mdl::MorphRecord& morph = model.morphs[i];
        _write(fh, morph.name, 0x14);
        _write(fh, &morph.offsetCount, 4);
        _write(fh, &morph.panel, 1);
        const mdl::PmdVertexMorphEntry* const base =
            model.morphs[0].vertexEntries;
        const std::int32_t baseCount = model.morphs[0].offsetCount;
        for (std::int32_t k = 0; k < morph.offsetCount; ++k) {
            const mdl::PmdVertexMorphEntry& e = morph.vertexEntries[k];
            if (i == 0) {
                _write(fh, &e.vertexIndex, 4);  // morph 0: raw index
            } else if (baseCount != 0) {
                // later morphs: remap through morph 0's table; an unfound
                // index skips the dword but still writes its offsets
                std::int32_t j = 0;
                while (j < baseCount &&
                       base[j].vertexIndex != e.vertexIndex)
                    ++j;
                if (j < baseCount)
                    _write(fh, &j, 4);
            }
            _write(fh, e.offset, 12);
        }
    }

    // ---- facial display list ---------------------------------------------
    _write(fh, &model.facialFrameCount, 1);
    for (int k = 0; k < model.facialFrameCount; ++k)
        _write(fh, &model.displayFrames[k].targetIndex, 2);

    // ---- bone group names --------------------------------------------------
    // reader adds 2 (+1 with the facial group) back to this byte
    const bool hasFacial = model.facialFrameCount != 0;
    const std::uint8_t groupByte = static_cast<std::uint8_t>(
        model.groupCount - (hasFacial ? 3 : 2));
    _write(fh, &groupByte, 1);
    mdl::DisplayGroup* const groups = mdl::DisplayGroups(
        reinterpret_cast<unsigned char*>(&model));
    for (int m = hasFacial ? 3 : 2; m < model.groupCount; ++m)
        _write(fh, groups[m].name, 0x32);

    // ---- rigid-body display list -------------------------------------------
    _write(fh, &model.rigidBodyCount, 4);
    mdl::FrameGroup* const rbGroups = mdl::RigidGroups(
        reinterpret_cast<unsigned char*>(&model));
    for (std::uint32_t k = 0; k < model.rigidBodyCount; ++k) {
        const mdl::FrameGroup& rb = rbGroups[k];
        _write(fh, &rb.targetIndex, 2);
        // reader adds 1 (+1 with the facial group) back
        const std::uint8_t groupIdx = static_cast<std::uint8_t>(
            rb.groupIndex - (hasFacial ? 2 : 1));
        _write(fh, &groupIdx, 1);
    }

    // ---- English section ---------------------------------------------------
    const std::uint8_t hasEnglish = 1;
    _write(fh, &hasEnglish, 1);
    _write(fh, model.nameEn, 0x14);
    _write(fh, model.commentEn, 0x100);
    for (std::uint32_t i = 0; i < model.boneCount; ++i)
        _write(fh, model.boneTable[i].nameEn, 0x14);
    for (std::uint32_t i = 1; i < model.morphCount; ++i)  // skips morph 0
        _write(fh, model.morphs[i].nameEn, 0x14);
    for (int m = hasFacial ? 3 : 2; m < model.groupCount; ++m)
        _write(fh, groups[m].nameEn, 0x32);

    // ---- toon file names ----------------------------------------------------
    for (int i = 0; i < 10; ++i)
        _write(fh, model.pmdToonFileNames[i], 0x64);

    // ---- rigid bodies --------------------------------------------------------
    _write(fh, &model.rigidCount, 4);
    for (std::uint32_t i = 0; i < model.rigidCount; ++i) {
        const mdl::RigidRecord& rigid = model.rigidTable[i];
        _write(fh, rigid.name, 0x14);
        const std::uint16_t bone =
            static_cast<std::uint16_t>(rigid.boneIndex);
        _write(fh, &bone, 2);
        _write(fh, &rigid.group, 1);
        _write(fh, &rigid.noCollapse, 2);
        _write(fh, &rigid.shape, 1);
        _write(fh, rigid.size, 12);
        _write(fh, rigid.position, 12);
        _write(fh, rigid.rotation, 12);
        _write(fh, &rigid.mass, 4);
        _write(fh, &rigid.linearDamping, 4);
        _write(fh, &rigid.angularDamping, 4);
        _write(fh, &rigid.restitution, 4);
        _write(fh, &rigid.friction, 4);
        _write(fh, &rigid.mode, 1);
    }

    // ---- joints ---------------------------------------------------------------
    _write(fh, &model.jointCount, 4);
    for (std::uint32_t i = 0; i < model.jointCount; ++i) {
        const mdl::JointRecord& joint = model.jointTable[i];
        _write(fh, joint.name, 0x14);
        _write(fh, &joint.rigidA, 4);
        _write(fh, &joint.rigidB, 4);
        _write(fh, joint.position, 12);
        _write(fh, joint.rotation, 12);
        _write(fh, &joint.limits[3], 12);
        _write(fh, &joint.limits[0], 12);
        _write(fh, &joint.limits[9], 12);
        _write(fh, &joint.limits[6], 12);
        _write(fh, joint.springs, 24);
    }

    if (_commit(fh) != 0) writeFailed = true;
    if (_close(fh) != 0) writeFailed = true;
    if (writeFailed || !MoveFileExW(temporary, destination,
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        RefusePmd(app, PmdRefusal::Write);
        return;
    }
    wcscpy_s(model.path, 0x100, path);
}

// ===========================================================================
// 0x4A4850 (x64 sub_7FF7CB4F2240) - SetModelColor (was Sub4A4850):
// set model colour (ground-shadow tint sweep; called from pmm_load_v1/v2
// and the menu-286 colour picker).  Skips models whose physicsMode byte is
// 2, then locks the 16-byte-stride secondary vertex buffer (FVF 0x42:
// position + diffuse) and fills every vertex's colour with opaque
// ARGB(r, g, b).
// ===========================================================================
void SetModelColor(MMDApp* modelPtr, int r, int g, int b) {
    mdl::ModelRecord& model =
        *mdl::Mdl(reinterpret_cast<unsigned char*>(modelPtr));
    if (model.physicsMode == 2)
        return;
    if (model.vertexCount == 0)
        return;
    IDirect3DVertexBuffer9* vb =
        mdl::ResourceAs<IDirect3DVertexBuffer9>(model.vertexBuffer2);
    if (vb == nullptr)
        return;
    void* bits = nullptr;
    vb->Lock(0, 16 * model.vertexCount, &bits, 0);
    if (bits == nullptr) {
        vb->Unlock();
        return;
    }
    // the original widens r to a full register but takes g/b as bytes
    const std::uint32_t color = 0xFF000000u |
                                (static_cast<std::uint32_t>(r) << 16) |
                                (static_cast<std::uint32_t>(g & 0xFF) << 8) |
                                static_cast<std::uint32_t>(b & 0xFF);
    for (std::uint32_t i = 0; i < model.vertexCount; ++i) {
        *reinterpret_cast<std::uint32_t*>(
            static_cast<unsigned char*>(bits) + 16 * i + 12) = color;
    }
    vb->Unlock();
}

// ---- not-yet-ported original call targets with NO stub elsewhere -------
// (was Sub439E40/43A650/43B720/43BB30 - now InsertBoneCameraFrameLine /
//  DeleteBoneCameraFrameLine / InsertFacialLightFrameLine /
//  DeleteFacialLightFrameLine, the four frame-line edit commands in
//  src/window/frame_line_edit.cpp; declared in ported_funcs.hpp.)

// ===========================================================================
// 0x40B5A0 (x64 sub_7FF7CB43AB40) - menu-bar language refresh after the
// Japanese/English switch (menu 260).  Re-titles all eight top menus and
// every menu item with the EN literals / JP Shift-JIS strings of the
// original (the "full screen" item becomes "NVIDIA 3D Vision" when the
// renderer's stereo flag is set), then redraws the menu bar.
// ===========================================================================
namespace {

struct MenuText {
    int pos;
    const char* en;
    const char* jp;
    const wchar_t* zh;  // Simplified Chinese (port addition)
    // Stable command of the item, when it has one.  Runtime-inserted items
    // move the positions inside a popup, so a lookup by command id is the
    // only one that stays correct after they arrive.  0 keeps the original
    // positional lookup for entries nothing can shift.
    UINT command = 0;
    // A popup item has no command of its own.  subCommand names one command
    // inside that item's submenu and identifies it that way, so it survives
    // whatever else is inserted into the same popup.
    UINT subCommand = 0;
};


// Match only the direct commands of each child. A nested plugin submenu with
// the same command must not be mistaken for the host's own popup.
int ChildPopupWithCommand(HMENU parent, UINT command) {
    for (int i = 0; i < GetMenuItemCount(parent); ++i) {
        HMENU child = GetSubMenu(parent, i);
        for (int j = 0; child && j < GetMenuItemCount(child); ++j)
            if (GetMenuItemID(child, j) == command) return i;
    }
    return -1;
}

void ApplyMenuText(HMENU popup, const MenuText& item, MENUITEMINFOW& info) {
    if (item.command != 0) {
        SetMenuItemInfoW(popup, item.command, FALSE, &info);
    } else if (item.subCommand != 0) {
        const int position = ChildPopupWithCommand(popup, item.subCommand);
        // A missing popup must not overwrite an unrelated positional entry.
        if (position >= 0) SetMenuItemInfoW(popup, UINT(position), TRUE, &info);
    } else {
        SetMenuItemInfoW(popup, UINT(item.pos), TRUE, &info);
    }
}

constexpr MenuText kFileMenu[] = {
    {0, "New(&N)", "\x90\x56\x8b\x4b\x28\x26\x4e\x29", L"新建(&N)", 204 },
    {1, "Open(&O)", "\x8a\x4a\x82\xad\x28\x26\x4f\x29", L"打开(&O)", 205 },
    {2, "Save(&S)", "\x8f\xe3\x8f\x91\x82\xab\x95\xdb\x91\xb6\x28\x26\x53\x29", L"保存(&S)", 207 },
    {3, "Save as(&A)",
     "\x96\xbc\x91\x4f\x82\xf0\x95\x74\x82\xaf\x82\xc4\x95\xdb\x91\xb6"
     "\x28\x26\x41\x29", L"另存为(&A)", 208 },
    {5, "Render to video file(&V)",
     "\x93\xae\x89\xe6\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xc9\x8f\x6f\x97"
     "\xcd\x28\x26\x56\x29", L"导出视频(&V)", 223 },
    {6, "Render to picture file(&B)",
     "\x89\xe6\x91\x9c\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xc9\x8f\x6f"
     "\x97\xcd\x28\x26\x42\x29", L"导出图片(&B)", 276 },
    {8, "Load pose data(&P)",
     "\x83\x7c\x81\x5b\x83\x59\x83\x66\x81\x5b\x83\x5e\x93\xc7\x8d\x9e"
     "\x28\x26\x50\x29", L"载入姿势数据(&P)", 202 },
    {9, "Save pose data(&Q)",
     "\x83\x7c\x81\x5b\x83\x59\x83\x66\x81\x5b\x83\x5e\x95\xdb\x91\xb6"
     "\x28\x26\x51\x29", L"保存姿势数据(&Q)", 203 },
    {0xB, "Load motion data(&M)",
     "\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x83\x66\x81\x5b\x83\x5e"
     "\x93\xc7\x8d\x9e\x28\x26\x4d\x29", L"载入动作数据(&M)", 209 },
    {0xC, "Save motion data(&L)",
     "\x83\x82\x81\x5b\x83\x56\x83\x87\x83\x93\x83\x66\x81\x5b\x83\x5e"
     "\x95\xdb\x91\xb6\x28\x26\x4c\x29", L"保存动作数据(&L)", 210 },
    {0xE, "Load audio file(&W)",
     "\x57\x41\x56\x83\x74\x83\x40\x83\x43\x83\x8b\x93\xc7\x8d\x9e\x28"
     "\x26\x57\x29", L"载入音频(&W)", 206 },
    {0xF, "Play WAV with frame(&F)",
     "\xcc\xda\xb0\xd1\x88\xda\x93\xae\x8e\x9e\x57\x41\x56\x82\xf0\x96"
     "\xc2\x82\xe7\x82\xb7\x28\x26\x46\x29", L"移动帧时播放音频(&F)", 284 },
    {0x10, "Not play WAV file(&D)",
     "\x57\x41\x56\x83\x74\x83\x40\x83\x43\x83\x8b\x82\xf0\x96\xc2\x82"
     "\xe7\x82\xb3\x82\xc8\x82\xa2\x28\x26\x44\x29", L"禁用音频播放(&D)", 297 },
    {0x14, "Exit(&X)", "\x8f\x49\x97\xb9\x28\x26\x58\x29", L"退出(&X)", 200 },
};

constexpr MenuText kEditMenu[] = {
    {0, "Bone camera numeric input(&O)",
     "\x83\x7b\x81\x5b\x83\x93\x81\x45\x83\x4a\x83\x81\x83\x89\x90\x94"
     "\x92\x6c\x93\xfc\x97\xcd\x28\x26\x4f\x29", L"骨骼 / 相机数值输入(&O)", 300 },
    {1, "Bone camera angle initialize(&Z)",
     "\x83\x7b\x81\x5b\x83\x93\x81\x45\x83\x4a\x83\x81\x83\x89\x8a\x70"
     "\x93\x78\x30\x89\xbb\x28\x26\x5a\x29", L"重置骨骼 / 相机角度(&Z)", 302 },
    {3, "Delete unused frame(&D)",
     "\x95\x73\x97\x76\xcc\xda\xb0\xd1\x8d\xed\x8f\x9c\x28\x26\x44\x29", L"删除未使用的帧(&D)", 222 },
    {5, "Select all camera frame(&C)",
     "\xb6\xd2\xd7\xcc\xda\xb0\xd1\x82\xb7\x82\xd7\x82\xc4\x91\x49\x91"
     "\xf0\x28\x26\x43\x29", L"选择全部相机帧(&C)", 237 },
    {6, "Select all light frame(&L)",
     "\x8f\xc6\x96\xbe\xcc\xda\xb0\xd1\x82\xb7\x82\xd7\x82\xc4\x91\x49"
     "\x91\xf0\x28\x26\x4c\x29", L"选择全部照明帧(&L)", 238 },
    {7, "Select all self shadow frame(&S)",
     "\xbe\xd9\xcc\x89\x65\xcc\xda\xb0\xd1\x82\xb7\x82\xd7\x82\xc4\x91"
     "\x49\x91\xf0\x28\x26\x53\x29", L"选择全部自阴影帧(&S)", 239 },
    {8, "Select all gravity frame(&V)",
     "\x8f\x64\x97\xcd\xcc\xda\xb0\xd1\x82\xb7\x82\xd7\x82\xc4\x91\x49"
     "\x91\xf0\x28\x26\x56\x29", L"选择全部重力帧(&V)", 240 },
    {9, "Select all accessory frame(&A)",
     "\xb1\xb8\xbe\xbb\xd8\xcc\xda\xb0\xd1\x82\xb7\x82\xd7\x82\xc4\x91"
     "\x49\x91\xf0\x28\x26\x41\x29", L"选择全部配件帧(&A)", 241 },
    {0xB, "Multiply of camera frame position-angle(&G)",
     "\xb6\xd2\xd7\xcc\xda\xb0\xd1\x88\xca\x92\x75\x8a\x70\x93\x78\x95"
     "\xe2\x90\xb3\x28\x26\x47\x29", L"缩放相机帧的位置和角度(&G)", 242 },
    {0xD, "Select all bone frame(&N)",
     "\xce\xde\xb0\xdd\xcc\xda\xb0\xd1\x82\xb7\x82\xd7\x82\xc4\x91\x49"
     "\x91\xf0\x28\x26\x4e\x29", L"选择全部骨骼帧(&N)", 217 },
    {0xE, "Select all facial frames(&E)",
     "\x95\x5c\x8f\xee\xcc\xda\xb0\xd1\x82\xb7\x82\xd7\x82\xc4\x91\x49"
     "\x91\xf0\x28\x26\x45\x29", L"选择全部表情帧(&E)"},
    {0xF, "Select all disp/IK/OP frame(&M)",
     "\x95\x5c\x8e\xa6\xa5\x49\x4b\xa5\x8a\x4f\x90\x65\xcc\xda\xb0\xd1"
     "\x82\xb7\x82\xd7\x82\xc4\x91\x49\x91\xf0\x28\x26\x4d\x29", L"选择全部显示 / IK / 外亲帧(&M)", 218 },
    {0x11, "Paste to different flame(F_key)",
     "\x95\xca\xcc\xda\xb0\xd1\x82\xd6\xcd\xdf\xb0\xbd\xc4\x28\x46\xb7"
     "\xb0\x29", L"粘贴到指定帧(F键)", 250 },
    {0x13, "Insert frame line(bone or camera)(I_key)",
     "\x8b\xf3\xcc\xda\xb0\xd1\x91\x7d\x93\xfc\x28\xce\xde\xb0\xdd\x6f"
     "\x72\xb6\xd2\xd7\x29\x28\x49\xb7\xb0\x29", L"插入帧列（骨骼 / 相机）(I键)", 255 },
    {0x14, "Delete frame line(bone or camera)(K_key)",
     "\x97\xf1\xcc\xda\xb0\xd1\x8d\xed\x8f\x9c\x28\xce\xde\xb0\xdd\x6f"
     "\x72\xb6\xd2\xd7\x29\x28\x4b\xb7\xb0\x29", L"删除帧列（骨骼 / 相机）(K键)", 256 },
    {0x15, "Insert frame line(facial or light)(U_key)",
     "\x8b\xf3\xcc\xda\xb0\xd1\x91\x7d\x93\xfc\x28\x95\x5c\x8f\xee\x6f"
     "\x72\x8f\xc6\x96\xbe\x29\x28\x55\xb7\xb0\x29", L"插入帧列（表情 / 照明）(U键)", 257 },
    {0x16, "Delete frame line(facial or light)(J_key)",
     "\x97\xf1\xcc\xda\xb0\xd1\x8d\xed\x8f\x9c\x28\x95\x5c\x8f\xee\x6f"
     "\x72\x8f\xc6\x96\xbe\x29\x28\x4a\xb7\xb0\x29", L"删除帧列（表情 / 照明）(J键)", 258 },
    {0x18, "Multiply of bone frame position-angle(R_key)",
     "\xce\xde\xb0\xdd\xcc\xda\xb0\xd1\x88\xca\x92\x75\x8a\x70\x93\x78"
     "\x95\xe2\x90\xb3\x28\x52\xb7\xb0\x29", L"调整骨骼帧的位置和角度(R键)", 251 },
    {0x19, "Multiply of facial expression(&T)",
     "\x95\x5c\x8f\xee\x91\xe5\x82\xab\x82\xb3\x95\xe2\x90\xb3\x28\x26"
     "\x54\x29", L"缩放表情数值(&T)", 252 },
    {0x1B, "Apply center position bias(&B)",
     "\xbe\xdd\xc0\xb0\x88\xca\x92\x75\xca\xde\xb2\xb1\xbd\x95\x74\x89"
     "\xc1\x28\x26\x42\x29", L"调整中心位置偏移(&B)", 219 },
};

constexpr MenuText kViewMenu[] = {
    {0, "Screen size(&O)",
     "\x8f\x6f\x97\xcd\x83\x54\x83\x43\x83\x59\x28\x26\x4f\x29", L"画面尺寸(&O)", 212 },
    {2, "Separate window(&W)", "\x95\xca\x91\x8b\x28\x26\x57\x29", L"独立窗口(&W)", 280 },
    {5, "Camera & lighting tracking(&C)",
     "\xd3\xc3\xde\xd9\x95\xd2\x8f\x57\x8e\x9e\xb6\xd2\xd7\xa5\x8f\xc6"
     "\x96\xbe\x92\xc7\x8f\x5d\x28\x26\x43\x29", L"相机与照明追踪(&C)", 247 },
    {7, "Information display(&D)",
     "\x8f\xee\x95\xf1\x95\x5c\x8e\xa6\x28\x26\x44\x29", L"显示信息(&D)", 211 },
    {8, "Display coordinate axis(&G)",
     "\x8d\xc0\x95\x57\x8e\xb2\x95\x5c\x8e\xa6\x28\x26\x47\x29", L"显示坐标轴(&G)", 215 },
    {0xA, "Display ground shadow(&S)",
     "\x92\x6e\x96\xca\x89\x65\x95\x5c\x8e\xa6\x28\x26\x53\x29", L"显示地面阴影(&S)", 221 },
    {0xB, "Ground shadow color(&X)",
     "\x92\x6e\x96\xca\x89\x65\x90\x46\x90\xdd\x92\xe8\x28\x26\x58\x29", L"地面阴影亮度(&X)", 248 },
    {0xC, "Transparent ground shadow(&T)",
     "\x92\x6e\x96\xca\x89\x65\x90\x46\x93\xa7\x96\xbe\x89\xbb\x28\x26"
     "\x54\x29", L"透明地面阴影(&T)", 254 },
    {0xE, "Character transparent mode(V_key)",
     "\x94\xbc\x93\xa7\x96\xbe\x89\xbb\x28\x56\xb7\xb0\x29", L"模型半透明(V键)", 214 },
    {0xF, "Character Non-display mode(&A)",
     "\x83\x82\x83\x66\x83\x8b\x94\xf1\x95\x5c\x8e\xa6\x28\x26\x41\x29", L"隐藏模型(&A)", 283 },
    {0x11, "Thickness of edge line(&E)",
     "\x83\x47\x83\x62\x83\x57\x91\xbe\x82\xb3\x28\x26\x45\x29", L"描边粗细(&E)", 253 },
    {0x12, "Edge line color(&B)",
     "\x83\x47\x83\x62\x83\x57\x90\x46\x28\x26\x42\x29", L"描边颜色(&B)", 286 },
    {0x14, "Anti-aliasing(&H)",
     "\x83\x41\x83\x93\x83\x60\x83\x47\x83\x43\x83\x8a\x83\x41\x83\x58"
     "\x28\x26\x48\x29", L"抗锯齿(&H)", 277 },
    {0x16, "Mipmap(anisotropic)(&M)",
     "\x83\x7e\x83\x62\x83\x76\x83\x7d\x83\x62\x83\x76\x28\x88\xd9\x95"
     "\xfb\x90\xab\x83\x74\x83\x42\x83\x8b\x83\x5e\x29\x28\x26\x4d\x29", L"多级纹理（各向异性过滤）(&M)", 298 },
    {0x18, "Self-shadow(&P)",
     "\x83\x5a\x83\x8b\x83\x74\x83\x56\x83\x83\x83\x68\x83\x45\x95\x5c"
     "\x8e\xa6\x28\x26\x50\x29", L"自阴影(&P)", 279 },
    {0x1A, "Wire frame(&R)",
     "\x83\x8f\x83\x43\x83\x84\x81\x5b\x83\x74\x83\x8c\x81\x5b\x83\x80"
     "\x95\x5c\x8e\xa6\x28\x26\x52\x29", L"线框(&R)", 287 },
    {0x1C, "Full screen(Alt+Enter)",  // replaced below when stereo is on
     "\x83\x74\x83\x8b\x83\x58\x83\x4e\x83\x8a\x81\x5b\x83\x93\x95\x5c"
     "\x8e\xa6\x28\x41\x6c\x74\x2b\x45\x6e\x74\x65\x72\x29", L"全屏(Alt+Enter)", 290 },
    {0x1E, "Fps no limit", "\x66\x70\x73\x96\xb3\x90\xa7\x8c\xc0", L"不限帧率", 234 },
    {0x1F, "Max fps restricted to 30fps",
     "\x33\x30\x66\x70\x73\x90\xa7\x8c\xc0", L"限制为30帧/秒", 235 },
    {0x20, "Max fps restricted to 60fps",
     "\x36\x30\x66\x70\x73\x90\xa7\x8c\xc0", L"限制为60帧/秒", 236 },
    {0x22, "Save CPU power",
     "\x8f\xc8\x83\x47\x83\x6c\x83\x82\x81\x5b\x83\x68", L"节省内存模式", 299 },
};

constexpr MenuText kBackgroundMenu[] = {
    {0, "Accessories edit(&A)",
     "\x83\x41\x83\x4e\x83\x5a\x83\x54\x83\x8a\x95\xd2\x8f\x57\x28\x26"
     "\x41\x29", L"配件编辑(&A)", 249 },
    {1, "Model draw order(&O)",
     "\x83\x82\x83\x66\x83\x8b\x95\x60\x89\xe6\x8f\x87\x28\x26\x4f\x29", L"模型绘制顺序(&O)", 288 },
    {2, "Model calculate order(&C)",
     "\x83\x82\x83\x66\x83\x8b\x8c\x76\x8e\x5a\x8f\x87\x28\x26\x43\x29", L"模型计算顺序(&C)", 289 },
    {4, "Black background(&D)",
     "\x94\x77\x8c\x69\x8d\x95\x89\xbb\x28\x26\x44\x29", L"黑色背景(&D)", 282 },
    {6, "Load background video(&L)",
     "\x94\x77\x8c\x69\x41\x56\x49\x83\x74\x83\x40\x83\x43\x83\x8b\x93"
     "\xc7\x8d\x9e\x28\x26\x4c\x29", L"载入背景视频(&L)", 213 },
    {7, "Load background picture file(&R)",
     "\x94\x77\x8c\x69\x89\xe6\x91\x9c\x83\x74\x83\x40\x83\x43\x83\x8b"
     "\x93\xc7\x8d\x9e\x28\x26\x52\x29", L"载入背景图片(&R)", 232 },
    {9, "Show background AVI file(&A)",
     "\x94\x77\x8c\x69\x41\x56\x49\x95\x5c\x8e\xa6\x28\x26\x41\x29", L"显示背景视频(&A)", 216 },
    {0xA, "Show background picture file(&P)",
     "\x94\x77\x8c\x69\x89\xe6\x91\x9c\x95\x5c\x8e\xa6\x28\x26\x50\x29", L"显示背景图片(&P)", 233 },
    {0xC, "Screen capture mode OFF(&V)",
     "\xbd\xb8\xd8\xb0\xdd\x97\x70\xb7\xac\xcc\xdf\xc1\xac\x4f\x46\x46"
     "\x28\x26\x56\x29", L"关闭屏幕捕获(&V)", 243 },
    {0xD, "ON.mode01(&M)",
     "\x4f\x4e\xa5\x83\x82\x81\x5b\x83\x68\x82\x50\x28\x91\x53\x89\xe6"
     "\x96\xca\x29\x20\x28\x26\x4d\x29", L"开启（全画面模式）(&M)", 244 },
    {0xE, "ON.mode02(&N)",
     "\x4f\x4e\xa5\x83\x82\x81\x5b\x83\x68\x82\x51\x28\x34\x3a\x33\x94"
     "\xe4\x97\xa6\x29\x20\x28\x26\x4e\x29", L"开启（直接模式）(&N)", 245 },
    {0xF, "ON.mode03(&B)",
     "\x4f\x4e\xa5\x83\x82\x81\x5b\x83\x68\x82\x52\x28\x94\x77\x8c\x69"
     "\x41\x56\x49\x29\x20\x28\x26\x42\x29", L"开启（仅作为屏幕投影）(&B)", 246 },
};

constexpr MenuText kFacialMenu[] = {
    {0, "Delete lip frame",
     "\x83\x8a\x83\x62\x83\x76\x83\x74\x83\x8c\x81\x5b\x83\x80\x91\x53"
     "\x82\xc4\x8d\xed\x8f\x9c", L"删除全部嘴部帧", 226 },
    {1, "Lip-sync with .VSQ file",
     "\x76\x73\x71\x82\xc9\x82\xe6\x82\xe9\x83\x8a\x83\x62\x83\x76\x83"
     "\x56\x83\x93\x83\x4e", L"从VSQ文件生成口型", 224 },
    {2, "Time shifting mouse frame",
     "\x83\x8a\x83\x62\x83\x76\x83\x74\x83\x8c\x81\x5b\x83\x80\x8e\x9e"
     "\x8a\xd4\x83\x56\x83\x74\x83\x67", L"偏移嘴部帧时间", 225 },
    {4, "Delete all eye frame",
     "\x96\xda\x83\x74\x83\x8c\x81\x5b\x83\x80\x91\x53\x82\xc4\x8d\xed"
     "\x8f\x9c", L"删除全部眼部帧", 228 },
    {5, "Randomly register blinking",
     "\x82\xdc\x82\xce\x82\xbd\x82\xab\x83\x89\x83\x93\x83\x5f\x83\x80"
     "\x93\x6f\x98\x5e", L"随机登记眨眼", 227 },
    {7, "Delete all eyebrow frame",
     "\x82\xdc\x82\xe4\x83\x74\x83\x8c\x81\x5b\x83\x80\x91\x53\x82\xc4"
     "\x8d\xed\x8f\x9c", L"删除全部眉毛帧", 231 },
    {9, "Reset all facial value",
     "\x91\x53\x82\xc4\x82\xcc\x95\x5c\x8f\xee\x83\x8a\x83\x5a\x83\x62"
     "\x83\x67", L"重置全部表情数值", 230 },
    {0xB, "Regist all facial frame(H_key)",
     "\x91\x53\x82\xc4\x82\xcc\x95\x5c\x8f\xee\x83\x74\x83\x8c\x81\x5b"
     "\x83\x80\x93\x6f\x98\x5e\x28\x48\xb7\xb0\x29", L"登记全部表情帧(H键)", 229 },
};

constexpr MenuText kPhysicsMenu[] = {
    {0, "On/off mode(&O)",
     "\x83\x49\x83\x93\x2f\x83\x49\x83\x74\x83\x82\x81\x5b\x83\x68\x28"
     "\x26\x4f\x29", L"按开关状态计算(&O)", 269 },
    {1, "Anytime(&E)", "\x8f\xed\x82\xc9\x89\x89\x8e\x5a\x28\x26\x45\x29", L"始终计算(&E)", 265 },
    {2, "Trace mode(&T)",
     "\x83\x67\x83\x8c\x81\x5b\x83\x58\x83\x82\x81\x5b\x83\x68\x28\x26"
     "\x54\x29", L"跟踪模式(&T)", 270 },
    {3, "No calculation(&N)",
     "\x89\x89\x8e\x5a\x82\xb5\x82\xc8\x82\xa2\x28\x26\x4e\x29", L"不计算(&N)", 272 },
    {5, "Playtime use on/off mode(&P)",
     "\x8d\xc4\x90\xb6\x8e\x9e\x82\xcd\x8f\xed\x82\xc9\x83\x49\x83\x93"
     "\x2f\x83\x49\x83\x74\x83\x82\x81\x5b\x83\x68\x82\xc9\x82\xb7\x82"
     "\xe9\x28\x26\x50\x29", L"播放时使用开关模式(&P)", 271 },
    {7, "Display bodies(&D)",
     "\x8d\x84\x91\xcc\x95\x5c\x8e\xa6\x28\x26\x44\x29", L"显示刚体(&D)", 264 },
    {9, "Gravity setting(&G)",
     "\x8f\x64\x97\xcd\x90\xdd\x92\xe8\x28\x26\x47\x29", L"重力设置(&G)", 266 },
    {0xA, "Initialize bodies position(&I)",
     "\x8d\x84\x91\xcc\x88\xca\x92\x75\x8f\x89\x8a\xfa\x89\xbb\x28\x26"
     "\x49\x29", L"初始化刚体位置(&I)", 268 },
    {0xC, "Floor(&F)", "\x8f\xb0\x28\x26\x46\x29", L"地面碰撞(&F)", 285 },
    {0xE, "Select physical bone(&B)",
     "\x95\xa8\x97\x9d\x89\x65\x8b\xbf\x83\x7b\x81\x5b\x83\x93\x91\x49"
     "\x91\xf0\x28\x26\x42\x29", L"选择物理骨骼(&B)", 273 },
    {0xF, "Select physics ON frame(X mark)(&X)",
     "\x91\x53\x82\xc4\x82\xcc\x95\xa8\x97\x9d\x4f\x4e\x83\x74\x83\x8c"
     "\x81\x5b\x83\x80\x28\x58\x88\xf3\x29\x91\x49\x91\xf0\x28\x26\x58"
     "\x29", L"选择物理开启帧（X标记）(&X)", 274 },
    {0x10, "Change physics ON/OFF frame(&C)",
     "\x95\xa8\x97\x9d\x4f\x4e\x2f\x4f\x46\x46\x83\x74\x83\x8c\x81\x5b"
     "\x83\x80\x95\xcf\x8a\xb7\x28\x26\x43\x29", L"切换物理开关帧(&C)", 275 },
    {0x12, "About physical engine(&A)",
     "\x95\xa8\x97\x9d\x83\x47\x83\x93\x83\x57\x83\x93\x82\xc9\x82\xc2"
     "\x82\xa2\x82\xc4\x28\x26\x41\x29", L"关于物理引擎(&A)", 267 },
};

constexpr MenuText kMocapMenu[] = {
    {0, "Kinect(&K)", "Kinect(&K)", L"Kinect(&K)", 291 },  // one shared literal in the original
    {2, "Capture(&C)", "\xb7\xac\xcc\xdf\xc1\xac\x28\x26\x43\x29", L"捕获(&C)", 292 },
    {4, "L-R reversing(&R)",
     "\x8d\xb6\x89\x45\x94\xbd\x93\x5d\x28\x26\x52\x29", L"左右反转(&R)", 293 },
    {5, "Initialize lost bone(&L)",
     "\xdb\xbd\xc4\xce\xde\xb0\xdd\x8f\x89\x8a\xfa\x89\xbb\x28\x26\x4c"
     "\x29", L"重置丢失的骨骼(&L)", 294 },
    {6, "Display red man(&D)",
     "\x90\xd4\x90\x6c\x95\x5c\x8e\xa6\x28\x26\x44\x29", L"显示骨架(&D)", 295 },
    {8, "Load oni-file(&O)",
     "\x6f\x6e\x69\xcc\xa7\xb2\xd9\x93\xc7\x8d\x9e\x28\x26\x4f\x29", L"载入ONI文件(&O)", 296 },
};

constexpr MenuText kHelpMenu[] = {
    {0, "Chinese Mode(&C)", "English Mode(&E)", L"日语(&J)", 260 },
    {1, "Current model default language: Auto", "Current model default language: Auto", L"当前模型默认语言：自动", 310 },
    {2, "Model language auto-translation", "Model language auto-translation", L"模型语言自动翻译", 311 },
    {2, "Enhance model(&M)",
     "\x83\x82\x83\x66\x83\x8b\x8a\x67\x92\xa3\x28\x26\x4d\x29", L"模型扩展编辑(&M)", 0, 259 },
    {4, "Restore texture(&R)",
     "\xc3\xb8\xbd\xc1\xac\x93\xc7\x92\xbc\x82\xb5\x28\x26\x52\x29", L"重新载入纹理(&R)", 278 },
    {6, "About(&A)",
     "\xca\xde\xb0\xbc\xde\xae\xdd\x8f\xee\x95\xf1\x28\x26\x49\x29", L"关于(&A)", 201 },
    {3, "Keep separate window on top(&F)",
     "\x95\xca\x91\x8b\x8d\xc5\x91\x4f\x97\xf1\x95\x5c\x8e\xa6\x28\x26"
     "\x46\x29", L"独立窗口置顶(&F)", 281 },
    {0x12, "Set default folder to previous(&E)",
     "\xc3\xde\xcc\xab\xd9\xc4\xcc\xab\xd9\xc0\xde\x82\xf0\x91\x4f\x89"
     "\xf1\x88\xca\x92\x75\x82\xc6\x82\xb7\x82\xe9\x28\x26\x45\x29", L"记住上次打开的目录(&E)", 301 },
};

// the "enhance model" submenu below help item 2
constexpr MenuText kEnhanceMenu[] = {
    {0, "Edit English name(&E)",
     "\x89\x70\x8c\xea\x96\xbc\x95\xd2\x8f\x57\x28\x26\x45\x29", L"编辑英文名称(&E)", 259 },
    {1, "Toon texture(&T)",
     "\xc4\xa9\xb0\xdd\xc3\xb8\xbd\xc1\xac\x95\xcf\x8d\x58\x28\x26\x54"
     "\x29", L"卡通纹理(&T)", 261 },
    {2, "Physics model(&B)",
     "\x95\xa8\x97\x9d\x89\x89\x8e\x5a\x95\xd2\x8f\x57\x28\x26\x42\x29", L"物理模型(&B)", 262 },
    {4, "Save enhanced model(&P)",
     "\x8a\x67\x92\xa3\x83\x82\x83\x66\x83\x8b\x95\xdb\x91\xb6\x28\x26"
     "\x50\x29", L"保存扩展模型(&P)", 263 },
};

// Stable command of the original Help popup ("About", command 201 in both menu
// templates of res/templates/menu/menus.rc).  The port inserts its own popups
// directly in front of Help, so the popup is identified through this command
// instead of through its top-level index, which drifts by one as soon as the
// baking popup exists.
constexpr UINT kHelpAboutCommand = 201;
// MenuBlock::topPos sentinel: resolve the position from resolveCommand.
constexpr int kResolveByCommand = -1;
// Top-level position of the popup that owns `command`, or -1.
int TopLevelPositionWithCommand(HMENU menu, UINT command) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW entry{};
        entry.cbSize = sizeof(entry);
        entry.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, UINT(i), TRUE, &entry) || entry.hSubMenu == nullptr)
            continue;
        if (GetMenuState(entry.hSubMenu, command, MF_BYCOMMAND) != UINT(-1))
            return i;
    }
    return -1;
}

struct MenuBlock {
    int topPos;
    const char* titleEn;
    const char* titleJp;
    const wchar_t* titleZh;  // Simplified Chinese (port addition)
    const MenuText* items;
    int itemCount;
    // With topPos == kResolveByCommand the popup is located by this stable
    // command of its own submenu, so its position may drift.
    UINT resolveCommand = 0;
};

constexpr MenuBlock kMenus[] = {
    {0, "File(&F)",
     "\x83\x74\x83\x40\x83\x43\x83\x8b\x28\x26\x46\x29", L"\u6587\u4ef6(&F)", kFileMenu,
     static_cast<int>(sizeof(kFileMenu) / sizeof(kFileMenu[0]))},
    {1, "Edit(&D)",
     "\x95\xd2\x8f\x57\x28\x26\x44\x29", L"\u7f16\u8f91(&D)", kEditMenu,
     static_cast<int>(sizeof(kEditMenu) / sizeof(kEditMenu[0]))},
    {2, "View(&V)",
     "\x95\x5c\x8e\xa6\x28\x26\x56\x29", L"\u663e\u793a(&V)", kViewMenu,
     static_cast<int>(sizeof(kViewMenu) / sizeof(kViewMenu[0]))},
    {3, "Background(&B)",
     "\x94\x77\x8c\x69\x28\x26\x42\x29", L"\u80cc\u666f(&B)", kBackgroundMenu,
     static_cast<int>(sizeof(kBackgroundMenu) / sizeof(kBackgroundMenu[0]))},
    {4, "Facial expression(&M)",
     "\x95\x5c\x8f\xee\x28\x26\x4d\x29", L"\u8868\u60c5(&M)", kFacialMenu,
     static_cast<int>(sizeof(kFacialMenu) / sizeof(kFacialMenu[0]))},
    {5, "Physical operation(&P)",
     "\x95\xa8\x97\x9d\x89\x89\x8e\x5a\x28\x26\x50\x29", L"\u7269\u7406\u8ba1\u7b97(&P)", kPhysicsMenu,
     static_cast<int>(sizeof(kPhysicsMenu) / sizeof(kPhysicsMenu[0]))},
    {6, "Motion capture(&K)",
     "\xd3\xb0\xbc\xae\xdd\xb7\xac\xcc\xdf\xc1\xac\x28\x26\x4b\x29",
     L"\u52a8\u4f5c\u6355\u6349(&K)", kMocapMenu,
     static_cast<int>(sizeof(kMocapMenu) / sizeof(kMocapMenu[0]))},
    {kResolveByCommand, "Help(&H)",
     "\x83\x77\x83\x8b\x83\x76\x28\x26\x48\x29", L"\u5e2e\u52a9(&H)", kHelpMenu,
     static_cast<int>(sizeof(kHelpMenu) / sizeof(kHelpMenu[0])), kHelpAboutCommand},
};

}  // namespace

namespace {

// ASCII -> UTF-16 in-place helper for the Chinese fallback (items whose zh
// translation is not filled in yet keep the English label).
void WideFromAscii(wchar_t* buf, int cap, const char* ascii) {
    int i = 0;
    for (; i < cap - 1 && ascii != nullptr && ascii[i] != '\0'; ++i)
        buf[i] = static_cast<unsigned char>(ascii[i]);
    buf[i] = L'\0';
}

// Shift-JIS byte string -> UTF-16.  The menu's JP literals are raw SJIS bytes
// taken verbatim from the original image; the ANSI (A) menu APIs decode them
// with the *system* code page, which garbles them on non-Japanese locales.
void SJisToWide(wchar_t* buf, int cap, const char* sjis) {
    if (sjis == nullptr) {
        if (cap > 0) buf[0] = L'\0';
        return;
    }
    const int n = MultiByteToWideChar(932 /*CP_SHIFT_JIS*/, 0, sjis, -1,
                                      buf, cap);
    if (n <= 0 && cap > 0)
        buf[0] = L'\0';
}

// The view menu no longer carries a separate "enable automatic transparent
// material sorting" item: the on/off state is the "off" entry of the sort-limit
// submenu, and command 303 keeps a compatibility command that swaps Off and
// Standard (see CmdViewMenu).  Removal is by command id, which leaves the
// position-based translations of the original view items where they are.
void RemoveLegacyAutoSortingMenuItem(MMDApp* app) {
    HWND hwnd = app->state.hwnd;
    HMENU menu = GetMenu(hwnd);
    if (menu == nullptr)
        return;
    HMENU sub = GetSubMenu(menu, 2);
    if (sub == nullptr)
        return;
    if (GetMenuState(sub, kAutoTransparentMaterialSortingMenuId, MF_BYCOMMAND) !=
        static_cast<UINT>(-1))
        DeleteMenu(sub, kAutoTransparentMaterialSortingMenuId, MF_BYCOMMAND);
}

// Transparent-sort-limit submenu (port addition).  The popup's own command id
// only identifies the parent item; the entries use ids 304..308, where the
// command id minus kTransparentSortPrecisionMenuFirst is the stored
// TransparentSortPrecision value: 304..307 are Low..Maximum and 308 is Off.
// The enum is the whole state - there is no second enable flag - and picking
// Off overwrites the stored level instead of remembering the previous one.
// The table is in display order (Off first), which is not the command-id
// order, so every item is addressed by command id.  Parent 32004 is distinct
// from the skinning commands (32000..32002 and 32100..32227).
constexpr UINT kTransparentSortPrecisionMenuId = 32004;

struct PrecisionItemText {
    UINT id;
    const wchar_t* zh;
    const wchar_t* en;
    const wchar_t* jp;
};
constexpr PrecisionItemText kPrecisionItemTexts[] = {
    {kTransparentSortPrecisionMenuOff, L"关闭（默认）", L"Off (default)", L"オフ（既定）"},
    {kTransparentSortPrecisionMenuFirst, L"低", L"Low", L"低"},
    {kTransparentSortPrecisionMenuFirst + 1, L"标准",
     L"Standard", L"標準"},
    {kTransparentSortPrecisionMenuFirst + 2, L"高", L"High", L"高"},
    {kTransparentSortPrecisionMenuFirst + 3, L"最高", L"Maximum", L"最高"},
};

// The command handler derives the stored value from an id by subtracting
// kTransparentSortPrecisionMenuFirst, and the check mark adds the stored value
// back onto the same base, so the enum order and the id order must not drift
// apart.  Low is the base, Maximum keeps the last value id and Off keeps the
// switch-off id, which follows the values.
static_assert(kTransparentSortPrecisionMenuOff ==
                  kTransparentSortPrecisionMenuLast + 1,
              "the sort-limit ids must stay contiguous");
static_assert(static_cast<UINT>(TransparentSortPrecision::Low) == 0,
              "Low must stay on the first sort-limit id");
static_assert(static_cast<UINT>(TransparentSortPrecision::Maximum) ==
                  static_cast<UINT>(kTransparentSortPrecisionMenuLast -
                                    kTransparentSortPrecisionMenuFirst),
              "Maximum must keep the last value id");
static_assert(static_cast<UINT>(TransparentSortPrecision::Off) ==
                  static_cast<UINT>(kTransparentSortPrecisionMenuOff -
                                    kTransparentSortPrecisionMenuFirst),
              "Off must keep the switch-off id");

}  // namespace

// Build or refresh the "transparent sort limit" submenu of the view menu.
// Every item is located by command id, so the position-based translations of
// the original items never move, and a zh/en/ja switch only rewrites the labels
// instead of appending a second copy.  The five entries are "off" followed by
// the four values, all drawn as radio items, and the single check mark comes
// straight from the stored TransparentSortPrecision value.
void RefreshTransparentSortPrecisionMenu(MMDApp* app) {
    if (app == nullptr)
        return;
#if MMD_ORIGINAL_PROFILE
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    if (HMENU menu = GetMenu(app->state.hwnd)) {
        if (HMENU view = GetSubMenu(menu, 2)) {
            DeleteMenu(view, kAutoTransparentMaterialSortingMenuId, MF_BYCOMMAND);
            DeleteMenu(view, kTransparentSortPrecisionMenuId, MF_BYCOMMAND);
        }
    }
    return;
#else
    HWND hwnd = app->state.hwnd;
    HMENU menu = GetMenu(hwnd);
    if (menu == nullptr)
        return;
    HMENU view = GetSubMenu(menu, 2);
    if (view == nullptr)
        return;
    const bool chinese = app->state.englishUI == 2;
    const bool english = app->state.englishUI == 1;
    const wchar_t* title = chinese ? L"透明排序上限"
                                   : (english ? L"Transparent sort limit"
                                              : L"透明ソート上限");
    HMENU popup = nullptr;
    MENUITEMINFOW miiw{};
    miiw.cbSize = sizeof(miiw);
    miiw.fMask = MIIM_SUBMENU;
    if (GetMenuItemInfoW(view, kTransparentSortPrecisionMenuId, FALSE, &miiw))
        popup = miiw.hSubMenu;
    if (popup == nullptr) {
        popup = CreatePopupMenu();
        if (popup == nullptr)
            return;
        miiw.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        miiw.wID = kTransparentSortPrecisionMenuId;
        miiw.hSubMenu = popup;
        miiw.dwTypeData = const_cast<wchar_t*>(title);
        if (!InsertMenuItemW(view, GetMenuItemCount(view), TRUE, &miiw)) {
            DestroyMenu(popup);
            return;
        }
    }
    miiw.fMask = MIIM_STRING;
    miiw.dwTypeData = const_cast<wchar_t*>(title);
    SetMenuItemInfoW(view, kTransparentSortPrecisionMenuId, FALSE, &miiw);
    // Entries outside the current table are dropped before the labels are
    // written, so a language switch leaves exactly the five current items
    // behind instead of a stale one from an earlier id set.
    for (int i = GetMenuItemCount(popup) - 1; i >= 0; --i) {
        const UINT itemId = GetMenuItemID(popup, static_cast<UINT>(i));
        bool known = false;
        for (const PrecisionItemText& text : kPrecisionItemTexts) {
            if (text.id == itemId) {
                known = true;
                break;
            }
        }
        if (!known)
            DeleteMenu(popup, static_cast<UINT>(i), MF_BYPOSITION);
    }
    for (const PrecisionItemText& text : kPrecisionItemTexts) {
        const wchar_t* label =
            chinese ? text.zh : (english ? text.en : text.jp);
        if (GetMenuState(popup, text.id, MF_BYCOMMAND) == static_cast<UINT>(-1))
            AppendMenuW(popup, MF_STRING | MFT_RADIOCHECK, text.id, label);
        miiw.fMask = MIIM_FTYPE | MIIM_STRING;
        miiw.fType = MFT_RADIOCHECK;
        miiw.dwTypeData = const_cast<wchar_t*>(label);
        SetMenuItemInfoW(popup, text.id, FALSE, &miiw);
    }
    // One check mark, taken from the stored value alone: the enum covers the
    // switch-off entry as well, so no extra flag is consulted.  That entry sits
    // first but holds the highest command id, so the state is set per item
    // instead of through CheckMenuRadioItem, whose command-id range walks menu
    // positions.
    const UINT selected =
        kTransparentSortPrecisionMenuFirst +
        static_cast<UINT>(app->TransparentMaterialSortPrecision());
    for (const PrecisionItemText& text : kPrecisionItemTexts) {
        CheckMenuItem(popup, text.id,
                      MF_BYCOMMAND | (text.id == selected ? MF_CHECKED
                                                          : MF_UNCHECKED));
    }
#endif
}

void RefreshMenuLanguage(MMDApp* app) {  // was Sub40B5A0, VA 0x0040B5A0
    HWND hwnd = app->state.hwnd;
    HMENU menu = GetMenu(hwnd);
    MENUITEMINFOW miiw{};
    miiw.cbSize = sizeof(miiw);
    miiw.fMask = 0x40 /*MIIM_STRING*/;
    const bool english = app->state.englishUI == 1;
    const bool chinese = app->state.englishUI == 2;
    wchar_t wideBuf[256];
    wchar_t titleBuf[256];

    for (const MenuBlock& block : kMenus) {
        // A popup whose index can drift is resolved from one of its own stable
        // commands; the popups before Help keep their original position.
        const int top = block.resolveCommand != 0
            ? TopLevelPositionWithCommand(menu, block.resolveCommand) : block.topPos;
        if (top < 0)
            continue;  // this menu bar does not carry that popup
        if (chinese && block.titleZh != nullptr) {
            miiw.dwTypeData = const_cast<wchar_t*>(block.titleZh);
        } else {
            if (english)
                WideFromAscii(titleBuf, 256, block.titleEn);
            else
                SJisToWide(titleBuf, 256, block.titleJp);
            miiw.dwTypeData = titleBuf;
        }
        // Change only the caption; ModifyMenu without MF_POPUP replaces
        // the popup with a command and loses its submenu and command state.
        SetMenuItemInfoW(menu, UINT(top), TRUE, &miiw);
        HMENU sub = GetSubMenu(menu, top);
        for (int i = 0; i < block.itemCount; ++i) {
            const MenuText& item = block.items[i];
            if (chinese && item.zh != nullptr) {
                miiw.dwTypeData = const_cast<wchar_t*>(item.zh);
            } else {
                if (chinese)
                    WideFromAscii(wideBuf, 256, item.en);
                else if (english)
                    WideFromAscii(wideBuf, 256, item.en);
                else
                    SJisToWide(wideBuf, 256, item.jp);
                miiw.dwTypeData = wideBuf;
            }
            ApplyMenuText(sub, item, miiw);
        }
    }

    // Keep this setting's caption independent from the UI language it overrides.
    {
        const int help = TopLevelPositionWithCommand(menu, kHelpAboutCommand);
        HMENU popup = help >= 0 ? GetSubMenu(menu, help) : nullptr;
        static const wchar_t* const labels[3][3] = {
            {L"当前模型默认语言：自动", L"当前模型默认语言：日语", L"当前模型默认语言：英语"},
            {L"Current model default language: Auto", L"Current model default language: Japanese", L"Current model default language: English"},
            {L"現在のモデルの既定言語：自動", L"現在のモデルの既定言語：日本語", L"現在のモデルの既定言語：英語"}
        };
        if (popup) {
            const unsigned pref = gModelInfoLanguagePreference;
            const unsigned ui = app->EnglishUI() == kUiEnglish ? 1 : app->EnglishUI() == kUiChinese ? 0 : 2;
            MENUITEMINFOW setting{}; setting.cbSize = sizeof(setting); setting.fMask = MIIM_STRING;
            setting.dwTypeData = const_cast<wchar_t*>(labels[ui][pref <= 2 ? pref : 0]);
            SetMenuItemInfoW(popup, 310, FALSE, &setting);
            const wchar_t* translationLabels[] = {
                L"模型语言自动翻译", L"Model language auto-translation", L"モデル名を自動翻訳"};
            setting.dwTypeData = const_cast<wchar_t*>(translationLabels[ui]);
            SetMenuItemInfoW(popup, 311, FALSE, &setting);
            CheckMenuItem(popup, 311, MF_BYCOMMAND |
                (gModelNameAutoTranslation ? MF_CHECKED : MF_UNCHECKED));
            EnableMenuItem(popup, 310, MF_BYCOMMAND | MF_ENABLED);
            EnableMenuItem(popup, 311, MF_BYCOMMAND | MF_ENABLED);
        }
    }

    // the full-screen item becomes the 3D-Vision variant when the renderer
    // has NVAPI stereo enabled
    D3DRenderer* renderer = app->Renderer();
    if (renderer != nullptr && renderer->stereoEnabled != 0) {
        const int view = TopLevelPositionWithCommand(menu, 212);
        HMENU sub = view >= 0 ? GetSubMenu(menu, view) : nullptr;
        if (sub == nullptr) return;
        if (chinese) {
            miiw.dwTypeData = const_cast<wchar_t*>(L"NVIDIA 3D Vision(Alt+Enter)");
        } else if (english) {
            miiw.dwTypeData = const_cast<wchar_t*>(L"NVIDIA 3D Vision(Alt+Enter)");
        } else {
            SJisToWide(wideBuf, 256,
                       "\x4e\x56\x49\x44\x49\x41\x20\x33\x44\x20\x56\x69"
                       "\x73\x69\x6f\x6e\x95\x5c\x8e\xa6\x28\x41\x6c\x74"
                       "\x2b\x45\x6e\x74\x65\x72\x29");
            miiw.dwTypeData = wideBuf;
        }
        // The full-screen item is addressed by its command: runtime-inserted
        // entries moved the position it used to sit at.
        SetMenuItemInfoW(sub, 290, FALSE, &miiw);
    }

    // Resolve both Help and its enhance-model child through stable commands.
    {
        const int help = TopLevelPositionWithCommand(menu, kHelpAboutCommand);
        HMENU popup = help >= 0 ? GetSubMenu(menu, help) : nullptr;
        const int position = popup ? ChildPopupWithCommand(popup, 259) : -1;
        HMENU sub = position >= 0 ? GetSubMenu(popup, position) : nullptr;
        if (sub != nullptr)
            for (const MenuText& item : kEnhanceMenu) {
                if (chinese && item.zh != nullptr) {
                    miiw.dwTypeData = const_cast<wchar_t*>(item.zh);
                } else {
                    if (chinese)
                        WideFromAscii(wideBuf, 256, item.en);
                    else if (english)
                        WideFromAscii(wideBuf, 256, item.en);
                    else
                        SJisToWide(wideBuf, 256, item.jp);
                    miiw.dwTypeData = wideBuf;
                }
            ApplyMenuText(sub, item, miiw);
            }
    }

    RefreshEffectMenuLanguage(app);
    antialias::RefreshMenu(app);
    image_sequence::RefreshMenu(app);
    mmv4mms::RefreshLanguage(app);
    plugin_menu::Refresh(app);
    RefreshSkinningMenu(app);
    bake::RefreshMenu(app);
    frame_rate_correction::RefreshMenu(app);
    sequence_scatter::RefreshMenu(app);
    RemoveLegacyAutoSortingMenuItem(app);
    RefreshTransparentSortPrecisionMenu(app);
    // Refresh the Help menu's corner label along with the interface language.
    viewport_corner_menu::EnsureItem(app);
    autosave::RefreshMenu(app);
    viewport_render_scale::RefreshMenu(app);
    DrawMenuBar(hwnd);
}

void SetModelExtensionMenuEnabled(MMDApp* app, bool enabled) {
    if (app == nullptr) return;
    HWND hwnd = app->MainWindow();
    if (hwnd == nullptr) hwnd = app->FloatingWindow();
    if (hwnd == nullptr) return;
    HMENU menu = GetMenu(hwnd);
    const int helpPosition = TopLevelPositionWithCommand(menu, kHelpAboutCommand);
    HMENU help = helpPosition >= 0 ? GetSubMenu(menu, helpPosition) : nullptr;
    // These are display preferences, not model-extension editing commands.
    // Restore them during model/panel transitions and every popup opening,
    // including when an earlier menu update left a disabled state behind.
    // EnableMenuItem preserves the current check mark.
    if (help) {
        EnableMenuItem(help, 310, MF_BYCOMMAND | MF_ENABLED);
        EnableMenuItem(help, 311, MF_BYCOMMAND | MF_ENABLED);
    }
    const int position = help ? ChildPopupWithCommand(help, 259) : -1;
    if (position < 0) return;
    EnableMenuItem(help, UINT(position), MF_BYPOSITION |
                   (enabled ? MF_ENABLED : MF_GRAYED));
    // Only popup item states changed. The native popup reads them before
    // painting; redrawing the whole bar here flashes every nested submenu.
}
// VA 0x0042AE20 - path copy: real port moved to src/media/media_load.cpp.

// ===========================================================================
// 0x4076E0 (x64 sub_7FF7CB428F30) - ReloadTextureCache (was Sub4076E0):
// renderer refresh after the toon reload (menu 278, thiscall on the
// renderer object).  Walks the shared 10000-entry texture cache and
// reloads every named entry from disk via D3DXCreateTextureFromFileExW
// (1024 mipmaps with the 1x1 no-mipmap retry), re-sampling the
// bottom-left pixel colour into the entry's tag bytes; entries that fail
// to reload keep a null texture with zeroed colour.
// ===========================================================================
void ReloadTextureCache(void* rendererArg) {
    InvalidateTextureLookup();
    D3DRenderer* renderer = static_cast<D3DRenderer*>(rendererArg);
    IDirect3DDevice9* device = renderer->device;
    auto* d3dx = &d3dx::Get();
    const bool haveD3dx = d3dx->Load() && d3dx->fromFileExW != nullptr;

    struct ImgInfo {  // first fields of D3DXIMAGE_INFO
        UINT Width, Height, Depth, MipLevels;
        UINT rest[16];
    };

    for (int i = 0; i < 10000; ++i) {
        ResourcePoolEntry& entry = renderer->resourcePool[i];
        if (entry.heapBuffer == nullptr)
            break;  // end of the used region
        auto* name = static_cast<wchar_t*>(entry.heapBuffer);
        auto*& texture =
            reinterpret_cast<IDirect3DTexture9*&>(entry.comObject);
        if (texture != nullptr) {
            texture->Release();
            texture = nullptr;
        }
        unsigned char* rgb =
            reinterpret_cast<unsigned char*>(&entry.tag);
        rgb[3] = kTextureAlphaKnown | kTextureUsesAlpha;
        if (!haveD3dx) {
            rgb[0] = rgb[1] = rgb[2] = 0;
            continue;
        }
        ImgInfo info = {};
        HRESULT hr = d3dx->fromFileExW(
            device, name, kD3dxDefault, kD3dxDefault, 1, 1024,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, kD3dxDefault, kD3dxDefault, 0,
            &info, nullptr, &texture);
        if (info.Width == 1 && info.Height == 1 && SUCCEEDED(hr)) {
            if (texture != nullptr) {
                texture->Release();
                texture = nullptr;
            }
            hr = d3dx->fromFileExW(
                device, name, kD3dxDefault, kD3dxDefault, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, kD3dxDefault,
                kD3dxDefault, 0, &info, nullptr, &texture);
        }
        if (FAILED(hr) &&
            FAILED(d3dx->fromFileExW(
                device, name, kD3dxDefault, kD3dxDefault, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, kD3dxDefault,
                kD3dxDefault, 0, &info, nullptr, &texture))) {
            texture = nullptr;
            rgb[0] = rgb[1] = rgb[2] = 0;
            continue;
        }
        CacheTextureColorAndAlpha(texture, rgb);
    }
}

// ---- helpers ported in other translation units (declared here with their
//      original VAs; not yet registered in ported_funcs.hpp) --------------

// VA 0x004629D0 - fullscreen enter/restore window manager; the real body
// now lives in src/app/avi_record_start.cpp (declared in ported_funcs.hpp).

}  // namespace mikumikustudio
