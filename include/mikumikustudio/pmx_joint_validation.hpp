#pragma once
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <string>

namespace mikumikustudio {
// Preserve source indices for diagnostics and exporting. A negative constraint
// ID disables the joint in the solver and the physics editor.
inline unsigned MissingJointBodies(const mdl::ModelRecord& model,
                                   const mdl::JointRecord& joint) {
    const auto missing = [&](int index) {
        return index < 0 || static_cast<unsigned>(index) >= model.rigidCount ||
               model.rigidTable == nullptr || model.rigidTable[index].body == nullptr;
    };
    return (missing(joint.rigidA) ? 1u : 0u) | (missing(joint.rigidB) ? 2u : 0u);
}
inline const wchar_t* PmxJointWarningTitle(unsigned char language) {
    return language == kUiChinese ? L"模型物理警告" :
           language == kUiJapanese ? L"モデルの物理警告" : L"Model physics warning";
}
inline std::wstring PmxJointWarnings(const mdl::ModelRecord& model,
                                    unsigned char language) {
    std::wstring lines;
    for (unsigned i = 0; i < model.jointCount; ++i) {
        const auto& joint = model.jointTable[i];
        const unsigned missing = MissingJointBodies(model, joint);
        if (!missing) continue;
        const wchar_t* name = UiUsesEnglishNames(language) && joint.enText && *joint.enText
            ? joint.enText : joint.jpText;
        if (!name) name = L"";
        if (language == kUiChinese) {
            lines += L"joint 点 " + std::to_wstring(i) + L" 号（" + name + L"）：";
            if (missing & 1) lines += L"刚体 A 未连接";
            if (missing == 3) lines += L"；";
            if (missing & 2) lines += L"刚体 B 未连接";
        } else if (language == kUiJapanese) {
            lines += L"ジョイント " + std::to_wstring(i) + L" 番（" + name + L"）：";
            if (missing & 1) lines += L"剛体 A が接続されていません";
            if (missing == 3) lines += L"；";
            if (missing & 2) lines += L"剛体 B が接続されていません";
        } else {
            lines += L"Joint #" + std::to_wstring(i) + L" (" + name + L"): ";
            if (missing & 1) lines += L"Rigid body A is not connected";
            if (missing == 3) lines += L"; ";
            if (missing & 2) lines += L"Rigid body B is not connected";
        }
        lines += L"\n";
    }
    if (lines.empty()) return {};
    const wchar_t* lead = language == kUiChinese
        ? L"以下 joint 点已无效化，模型继续加载（编号从 0 开始）：\n\n"
        : language == kUiJapanese
        ? L"以下のジョイントを無効化してモデルを読み込みました（番号は 0 から始まります）：\n\n"
        : L"The model was loaded with the following joints disabled (indices start at 0):\n\n";
    return std::wstring(lead) + lines;
}
}  // namespace mikumikustudio