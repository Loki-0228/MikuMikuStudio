#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/original_file_encoding.hpp"
#include "mikumikustudio/model.hpp"

namespace mikumikustudio::original_import {

inline void Refuse(void* owner, unsigned language, bool modelEdit = false) {
    const wchar_t* importMessages[] = {
        L"名前または参照パスが CP932 の文字・長さの制限を超えるため、読み込みを中止しました。文字と長さを確認してください。",
        L"Import cancelled: a name or referenced path exceeds the CP932 character or field-length limits. Check its characters and length.",
        L"已取消导入：名称或引用路径超出 CP932 字符或字段长度限制。请检查字符和长度。"};
    const wchar_t* editMessages[] = {
        L"このモデルは標準 PMD に完全に保存できないため、モデル編集を開始できません。最小版のモデル編集は PMD のみ対応しています。",
        L"Model editing cannot start because this model cannot be preserved in standard PMD. The minimal build supports PMD model editing only.",
        L"此模型无法完整保存为标准 PMD，无法进入模型编辑。最小包的模型编辑仅支持 PMD。"};
    runtime_log::Write("ORIGINAL_PROFILE %s refused", modelEdit ? "model edit" : "import");
    if (owner) MessageBoxW(static_cast<HWND>(owner),
        (modelEdit ? editMessages : importMessages)[language <= 2 ? language : 1],
        L"MMD", MB_OK | MB_ICONERROR);
    SetLastError(modelEdit ? ERROR_NOT_SUPPORTED : ERROR_NO_UNICODE_TRANSLATION);
}

inline bool Text(const wchar_t* value, std::size_t bytes = 255) {
    std::string encoded;
    return original_file_encoding::Encode(value, bytes, encoded);
}

inline bool Legacy(const char* value, std::size_t capacity, std::size_t maximum) {
    std::string encoded;
    return original_file_encoding::Name(value, capacity, nullptr, maximum, encoded);
}

inline bool Reference(const wchar_t* path, void* owner, unsigned language,
                      std::size_t bytes = 255) {
#if MMD_ORIGINAL_PROFILE
    if (!Text(path, bytes)) { Refuse(owner, language); return false; }
#endif
    return true;
}

// Check the full source name against every standard output that uses it.
// PMM matches the legacy mirror; VMD bone/morph fields contain only 15 bytes.
inline bool ModelNames(const mdl::ModelRecord& model) {
    std::string encoded;
    const auto name = [&](const char* raw, std::size_t size, const wchar_t* wide,
                          std::size_t maximum, bool identity = false) {
        return original_file_encoding::Name(raw, size, wide, maximum, encoded) &&
            (!identity || encoded == std::string(raw, strnlen_s(raw, size)));
    };
    if (!name(model.name, sizeof model.name, model.pmxTextBuffers[0], 20) ||
        !name(model.nameEn, sizeof model.nameEn, model.pmxTextBuffers[1], 20)) return false;
    for (unsigned i = 0; i < model.boneCount; ++i) {
        const auto& bone = model.boneTable[i];
        if (!name(bone.name, sizeof bone.name, bone.jpText, 15, true) ||
            !name(bone.nameEn, sizeof bone.nameEn, bone.enText, 19)) return false;
    }
    for (unsigned i = 0; i < model.morphCount; ++i) {
        const auto& morph = model.morphs[i];
        if (!name(morph.name, sizeof morph.name, morph.jpText, 15, true) ||
            !name(morph.nameEn, sizeof morph.nameEn, morph.enText, 19)) return false;
    }
    return true;
}

// The PMD writer also checks structural and texture-field limits. Sharing
// that check keeps import and entry to the model editor consistent with save.
bool Model(mdl::ModelRecord& model);
bool CanEdit(mdl::ModelRecord& model);
}
