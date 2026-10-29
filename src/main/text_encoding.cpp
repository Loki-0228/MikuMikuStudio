#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/model.hpp"
#include <cstring>
#include <cwchar>
#include <unordered_map>

namespace mikumikustudio::text_encoding {
namespace {
std::unordered_map<const mdl::ModelRecord*,MaterialNames> materialNames;
struct DisplayGroupText {
    const mdl::ModelRecord* owner;
    std::array<std::wstring, 2> names;
    std::array<std::string, 2> legacy;
};
std::unordered_map<const mdl::DisplayGroup*, DisplayGroupText> displayGroupNames;

std::wstring LegacyName(const char* text, std::size_t capacity, bool repair) {
    const std::string bounded(text, strnlen_s(text, capacity));
    const bool utf8 = HasUtf8Bom(bounded.c_str());
    const char* bytes = bounded.c_str() + (utf8 ? 3 : 0);
    const UINT codePage = utf8 ? CP_UTF8 : 932;
    std::wstring result;
    if (Decode(bytes, codePage, result) || !repair) return result;

    // An imported fixed-width name can stop at a DBCS lead byte. Preserve its
    // readable prefix for display, without changing the motion/plugin identity.
    if (!utf8 && !bounded.empty() &&
        IsDBCSLeadByteEx(932, static_cast<BYTE>(bounded.back()))) {
        const auto prefix = bounded.substr(0, bounded.size() - 1);
        if (Decode(prefix.c_str(), 932, result) && !result.empty())
            return result + L"…";
    }
    // A valid alternate language is tried before this path. If both fields are
    // damaged, show the surviving text with replacement glyphs instead of
    // dropping the entire name. Never guess a different unmarked code page.
    const int count = MultiByteToWideChar(codePage, 0, bytes, -1, nullptr, 0);
    if (count > 0) {
        result.resize(count);
        MultiByteToWideChar(codePage, 0, bytes, -1, result.data(), count);
        result.resize(count - 1);
    }
    return result;
}

std::wstring LegacyNamePair(const char* preferred, const char* alternate,
                            std::size_t capacity) {
    for (bool repair : {false, true}) {
        for (const char* text : {preferred, alternate}) {
            auto name = LegacyName(text, capacity, repair);
            if (!name.empty()) return name;
        }
    }
    return {};
}
}
void SetMaterialNames(const mdl::ModelRecord& model,MaterialNames names){materialNames[&model]=std::move(names);}
void ForgetMaterialNames(const mdl::ModelRecord& model){materialNames.erase(&model);}
std::wstring MaterialName(const mdl::ModelRecord& model,std::size_t index,bool english){
    const auto found=materialNames.find(&model);
    if(found==materialNames.end()||index>=found->second.size())return {};
    const auto& names=found->second[index];return names[english?1:0].empty()?names[english?0:1]:names[english?1:0];
}
bool HasUtf8Bom(const char* text) {
    return text && std::strncmp(text, "\xEF\xBB\xBF", 3) == 0;
}

bool Decode(const char* bytes, UINT codePage, std::wstring& result) {
    result.clear();
    if (!bytes) return false;
    const int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
                                         bytes, -1, nullptr, 0);
    if (!count) return false;
    result.resize(count);
    if (!MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes, -1,
                             result.data(), count)) {
        result.clear();
        return false;
    }
    result.resize(count - 1);
    return true;
}

bool EncodeExact(const wchar_t* text, UINT codePage, std::string& result) {
    result.clear();
    if (!text) text = L"";
    const UINT cp = codePage == CP_ACP ? GetACP() : codePage;
    const DWORD flags = cp == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL replaced = FALSE;
    BOOL* replacement = cp == CP_UTF8 ? nullptr : &replaced;
    int count = WideCharToMultiByte(cp, flags, text, -1, nullptr, 0, nullptr, replacement);
    if (!count || replaced) return false;
    result.resize(count);
    if (!WideCharToMultiByte(cp, flags, text, -1, result.data(), count, nullptr, replacement)
        || replaced) {
        result.clear();
        return false;
    }
    result.resize(count - 1);
    // Reject even reversible-looking best-fit substitutions (e.g. yen/backslash).
    std::wstring decoded;
    if (!Decode(result.c_str(), cp, decoded) || decoded != text) {
        result.clear();
        return false;
    }
    return true;
}

namespace {
bool PmxSjis(const wchar_t* text, std::string& bytes) {
    const int count=WideCharToMultiByte(932,0,text,-1,nullptr,0,nullptr,nullptr);
    if(!count)return false;
    bytes.resize(count); BOOL replaced=FALSE;
    if(!WideCharToMultiByte(932,0,text,-1,bytes.data(),count,nullptr,&replaced)||replaced)return false;
    bytes.resize(count-1);return true;
}
}
void EncodePmxName(char* dst, std::size_t capacity, const wchar_t* text) {
    if(!dst||!capacity)return;
    dst[0]=0;if(!text||!*text)return;
    std::string bytes;
    if(!PmxSjis(text,bytes) && !EncodeExact(text,CP_ACP,bytes) && !EncodeExact(text,CP_UTF8,bytes))return;
    strncpy_s(dst,capacity,bytes.c_str(),_TRUNCATE);
}
bool MatchesOldPmxName(const char* saved, const wchar_t* text, std::size_t capacity) {
    if(!saved||!text||!*text||!capacity)return false;
    std::string bytes;std::vector<char> old(capacity,0);
    if(PmxSjis(text,bytes))strncpy_s(old.data(),capacity,bytes.c_str(),_TRUNCATE);
    else {
        size_t converted=0;
        if(wcstombs_s(&converted,old.data(),capacity,text,_TRUNCATE)!=0)old[0]=0;
    }
    return strcmp(saved,old.data())==0;
}

namespace {
bool Store(char* dst, std::size_t capacity, const std::string& value) {
    if (!dst || !capacity) return false;
    std::memset(dst, 0, capacity);
    if (value.size() >= capacity) return false;
    std::memcpy(dst, value.data(), value.size());
    return true;
}
bool StoreUtf8(char* dst, std::size_t capacity, const wchar_t* text) {
    std::string encoded;
    if (!EncodeExact(text, CP_UTF8, encoded)) return Store(dst, capacity, ""), false;
    return Store(dst, capacity, "\xEF\xBB\xBF" + encoded);
}
}

bool EncodePath(char* dst, std::size_t capacity, const wchar_t* path) {
    std::string encoded;
    for (UINT cp : {932u, GetACP(), 936u, 950u, 949u})
        if (EncodeExact(path, cp, encoded) && encoded.size() < capacity)
            return Store(dst, capacity, encoded);
    return StoreUtf8(dst, capacity, path);
}

bool EncodeDisplay(char* dst, std::size_t capacity, const wchar_t* text) {
    std::string encoded;
    if (EncodeExact(text, CP_ACP, encoded) && encoded.size() < capacity)
        return Store(dst, capacity, encoded);
    return StoreUtf8(dst, capacity, text);
}

std::wstring Display(const char* text) {
    std::wstring result;
    if (HasUtf8Bom(text)) Decode(text + 3, CP_UTF8, result);
    else Decode(text, CP_ACP, result);
    return result;
}

std::wstring LegacyFilename(const char* text, const wchar_t* resolvedPath) {
    if (HasUtf8Bom(text)) return Display(text);
    const wchar_t* tail = std::wcsrchr(resolvedPath, L'\\');
    tail = tail ? tail + 1 : resolvedPath;
    std::wstring decoded;
    // Existence-resolved Unicode path disambiguates untagged Chinese/Japanese
    // filenames. Preserve custom labels rather than replacing them with a path.
    for (UINT cp : {GetACP(), 932u, 936u, 950u, 949u, 65001u})
        if (Decode(text, cp, decoded) && _wcsicmp(decoded.c_str(), tail) == 0)
            return decoded;
    return Display(text);
}

std::wstring ModelLabel(const mdl::ModelRecord& model, bool english) {
    const auto name = ModelName(model, english);
    if (!name.empty()) return name;
    const wchar_t* filename = model.path;
    for (const wchar_t* p = filename; *p; ++p)
        if (*p == L'\\' || *p == L'/') filename = p + 1;
    if (*filename) return filename;
    return {};
}

std::wstring ModelName(const mdl::ModelRecord& model, bool english) {
    const auto* wide = model.pmxTextBuffers[english ? 1 : 0];
    if (wide && *wide) return wide;
    wide = model.pmxTextBuffers[english ? 0 : 1];
    if (wide && *wide) return wide;
    const char* narrow = english ? model.nameEn : model.name;
    if (!*narrow) narrow = english ? model.name : model.nameEn;
    std::wstring result;
    if (HasUtf8Bom(narrow)) return Display(narrow);
    // PMD model names are Shift-JIS; unlike untagged paths there is no file
    // existence check that can disambiguate arbitrary customized model names.
    if (Decode(narrow, 932, result)) return result;
    return Display(narrow);
}

std::wstring MorphName(const mdl::MorphRecord& morph, bool english) {
    const wchar_t* wide = english ? morph.enText : morph.jpText;
    if (wide && *wide) return wide;
    wide = english ? morph.jpText : morph.enText;
    if (wide && *wide) return wide;
    // PMD/VMD names are CP932, independent of the Windows/UI language.
    return LegacyNamePair(english ? morph.nameEn : morph.name,
                          english ? morph.name : morph.nameEn, sizeof(morph.name));
}

std::wstring BoneName(const mdl::BoneRecord& bone, bool english) {
    const wchar_t* wide = english ? bone.enText : bone.jpText;
    if (wide && *wide) return wide;
    wide = english ? bone.jpText : bone.enText;
    if (wide && *wide) return wide;
    return LegacyNamePair(english ? bone.nameEn : bone.name,
                          english ? bone.name : bone.nameEn, sizeof(bone.name));
}

void SetDisplayGroupName(const mdl::ModelRecord& model, const mdl::DisplayGroup& group,
                         const wchar_t* native, const wchar_t* english) {
    displayGroupNames[&group] = {&model, {native ? native : L"", english ? english : L""},
        {std::string(group.name, strnlen_s(group.name, sizeof(group.name))),
         std::string(group.nameEn, strnlen_s(group.nameEn, sizeof(group.nameEn)))}};
}

void ForgetDisplayGroupNames(const mdl::ModelRecord& model) {
    for (auto it = displayGroupNames.begin(); it != displayGroupNames.end();) {
        if (it->second.owner == &model) it = displayGroupNames.erase(it);
        else ++it;
    }
}

std::wstring DisplayGroupName(const mdl::DisplayGroup& group, bool english) {
    const auto found = displayGroupNames.find(&group);
    const auto name = [&](int slot, bool repair) {
        const char* narrow = slot ? group.nameEn : group.name;
        const std::string bounded(narrow, strnlen_s(narrow, sizeof(group.name)));
        // The English-name editor can rewrite the legacy field. Do not hide
        // those edits behind the original PMX text.
        if (found != displayGroupNames.end() && bounded == found->second.legacy[slot])
            return found->second.names[slot];
        return LegacyName(narrow, sizeof(group.name), repair);
    };
    for (bool repair : {false, true}) {
        auto result = name(english ? 1 : 0, repair);
        if (result.empty()) result = name(english ? 0 : 1, repair);
        if (!result.empty()) return result;
    }
    return {};
}

int MessageBoxJp(void* owner, const char* body, const char* caption,
                 unsigned int flags) {
    std::wstring wideBody;
    std::wstring wideCaption;
    // The JP literals are byte-exact Shift-JIS; ASCII (the EN branch) decodes
    // identically under CP932, so both branches can share this path.
    if (!Decode(body, 932, wideBody)) wideBody = Display(body);
    if (!Decode(caption, 932, wideCaption)) wideCaption = Display(caption);
    return MessageBoxW(static_cast<HWND>(owner), wideBody.c_str(),
                       wideCaption.c_str(), flags);
}
}
