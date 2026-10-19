#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"

#include <unordered_map>

namespace mikumikustudio::text_encoding {
namespace {
struct NameEntry {
    const wchar_t* japanese;
    const wchar_t* english;
    const wchar_t* alias;
};

constexpr NameEntry kBones[] = {
#include "bone_name_dictionary.inc"
};
constexpr NameEntry kMorphs[] = {
#include "morph_name_dictionary.inc"
};

std::wstring FoldEnglish(std::wstring text) {
    // Independent of the process locale. Do not normalize Japanese names or
    // invent matches for custom suffixes, punctuation, and full-width digits.
    for (auto& ch : text)
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
    return text;
}

struct Dictionary {
    std::unordered_map<std::wstring, const NameEntry*> japanese;
    std::unordered_map<std::wstring, const NameEntry*> english;
    std::unordered_map<std::wstring, const NameEntry*> foldedEnglish;

    template <std::size_t N>
    explicit Dictionary(const NameEntry (&entries)[N]) {
        for (const auto& entry : entries) {
            // First definition wins when the supplied dictionary has duplicate
            // names. Japanese spellings take precedence over English aliases.
            japanese.emplace(entry.japanese, &entry);
            for (const auto* name : {entry.english, entry.alias}) {
                if (!*name) continue;
                english.emplace(name, &entry);
                foldedEnglish.emplace(FoldEnglish(name), &entry);
            }
        }
    }

    const NameEntry* Find(const std::wstring& name) const {
        auto found = japanese.find(name);
        if (found != japanese.end()) return found->second;
        found = english.find(name);
        if (found != english.end()) return found->second;
        found = foldedEnglish.find(FoldEnglish(name));
        return found != foldedEnglish.end() ? found->second : nullptr;
    }
};

const Dictionary& Bones() { static const Dictionary dictionary(kBones); return dictionary; }
const Dictionary& Morphs() { static const Dictionary dictionary(kMorphs); return dictionary; }

const NameEntry* Find(const Dictionary& dictionary, const std::wstring& preferred,
                      const std::wstring& alternate) {
    const auto* entry = dictionary.Find(preferred);
    return entry ? entry : dictionary.Find(alternate);
}

std::wstring Label(const NameEntry* entry, const std::wstring& original, bool english) {
    return entry ? (english ? entry->english : entry->japanese) : original;
}
}

std::wstring BoneLabel(const mdl::BoneRecord& bone, bool english) {
    const auto original = BoneName(bone, english);
    if (!gModelNameAutoTranslation) return original;
    return Label(Find(Bones(), original, BoneName(bone, !english)), original, english);
}

std::wstring MorphLabel(const mdl::MorphRecord& morph, bool english) {
    const auto original = MorphName(morph, english);
    if (!gModelNameAutoTranslation) return original;
    return Label(Find(Morphs(), original, MorphName(morph, !english)), original, english);
}

std::wstring DisplayGroupLabel(const mdl::DisplayGroup& group, bool english) {
    const auto original = DisplayGroupName(group, english);
    if (!gModelNameAutoTranslation) return original;
    const auto alternate = DisplayGroupName(group, !english);
    const auto* entry = Find(Bones(), original, alternate);
    if (!entry) entry = Find(Morphs(), original, alternate);
    return Label(entry, original, english);
}

std::wstring MaterialLabel(const mdl::ModelRecord& model, std::size_t index, bool english) {
    const auto original = MaterialName(model, index, english);
    if (!gModelNameAutoTranslation) return original;
    const auto alternate = MaterialName(model, index, !english);
    const auto* entry = Find(Bones(), original, alternate);
    if (!entry) entry = Find(Morphs(), original, alternate);
    return Label(entry, original, english);
}
}
