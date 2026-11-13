#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/text_encoding.hpp"
#include "../plugins/scene_resource_budget.hpp"
#include "mikumikustudio/model.hpp"
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <algorithm>
#include <map>
#include <exception>
#include <limits>

namespace mikumikustudio::text_encoding {
namespace {
std::unordered_map<const mdl::ModelRecord*,MaterialNames> materialNames;
struct DisplayGroupText {
    const mdl::ModelRecord* owner;
    std::array<std::wstring, 2> names;
    std::array<std::string, 2> legacy;
};
using DisplayGroupTextMap = std::map<const mdl::DisplayGroup*, DisplayGroupText>;
DisplayGroupTextMap displayGroupNames;

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
#if !MMD_ORIGINAL_PROFILE
void MeasureResources(const mdl::ModelRecord& model, plugins::resource_budget::Collector& out) {
    const auto found = materialNames.find(&model);
    if (found != materialNames.end()) {
        out.Add(&*found, 1, sizeof(*found) + 4 * sizeof(void*)); out.Vector(found->second);
        for (const auto& names : found->second) for (const auto& name : names) out.String(name);
    }
    for (const auto& entry : displayGroupNames) if (entry.second.owner == &model) {
        out.Add(&entry, 1, sizeof(entry) + 4 * sizeof(void*));
        for (const auto& text : entry.second.names) out.String(text);
        for (const auto& text : entry.second.legacy) out.String(text);
    }
}
#endif
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

void EncodePmxMirror(char* destination, std::size_t capacity, const wchar_t* text) {
    if (!destination || !capacity) return;
    destination[0] = '\0';
    if (text == nullptr || text[0] == L'\0') return;
    const int need = WideCharToMultiByte(932, 0, text, -1, nullptr, 0,
                                         nullptr, nullptr);
    char* tmp = need > 0 ? static_cast<char*>(operator new(need)) : nullptr;
    BOOL usedDefault = FALSE;
    const int n = need > 0 ? WideCharToMultiByte(932, 0, text, -1, tmp, need,
                                                nullptr, &usedDefault) : 0;
    if (n == 0 || usedDefault) {
        size_t dummy = 0;
        if (wcstombs_s(&dummy, destination, capacity, text, _TRUNCATE) != 0)
            destination[0] = '\0';
    } else {
        strncpy_s(destination, capacity, tmp, _TRUNCATE);
    }
    operator delete(tmp);
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
    std::vector<char> old(capacity,0);
    EncodePmxMirror(old.data(),capacity,text);
    const auto length=strnlen_s(saved,capacity);
    return length<capacity && length==strnlen_s(old.data(),capacity) &&
        std::memcmp(saved,old.data(),length)==0;
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

std::array<unsigned int, 6> PathDecodingOrder() {
    return {GetACP(), 932u, 936u, 950u, 949u, CP_UTF8};
}

namespace {
bool EncodePathImpl(char* dst, std::size_t capacity, const wchar_t* path, bool checkProjectCollision) {
    if (!path) path = L"";
    const auto resolvesToSource = [&](const std::string& encoded) {
        if (HasUtf8Bom(encoded.c_str())) return false;
        std::wstring decoded;
        for (const auto codePage : PathDecodingOrder()) {
            if (!Decode(encoded.c_str(), codePage, decoded)) continue;
            if (decoded == path) return true;
            const DWORD attributes = GetFileAttributesW(decoded.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                return false;
        }
        return false;
    };
    std::string encoded;
    bool triedUtf8 = false;
    for (UINT cp : {932u, GetACP(), 936u, 950u, 949u}) {
        if (!EncodeExact(path, cp, encoded) || encoded.size() >= capacity) continue;
        if (!checkProjectCollision || resolvesToSource(encoded)) return Store(dst, capacity, encoded);
        // A marker keeps the known collision unambiguous on other locales too.
        // A path near the fixed field limit can still try another safe spelling.
        if (!triedUtf8 && StoreUtf8(dst, capacity, path)) return true;
        triedUtf8 = true;
    }
    return !triedUtf8 && StoreUtf8(dst, capacity, path);
}
}

bool EncodePath(char* dst, std::size_t capacity, const wchar_t* path) {
    return EncodePathImpl(dst, capacity, path, false);
}

bool EncodeProjectPath(char* dst, std::size_t capacity, const wchar_t* path) {
    return EncodePathImpl(dst, capacity, path, true);
}

bool IsLegacyPathEncoding(const char* encoded, const wchar_t* path) {
    std::wstring decoded;
    return !HasUtf8Bom(encoded) && Decode(encoded, 932, decoded) && decoded == (path ? path : L"");
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
    for (const auto cp : PathDecodingOrder())
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

bool CaptureDisplayGroupText(const mdl::ModelRecord& model, const mdl::DisplayGroup& group, DisplayGroupTextState& out) {
    const auto found = displayGroupNames.find(&group);
    if (found != displayGroupNames.end() && found->second.owner != &model) return false;
    DisplayGroupTextState next;
    if (found != displayGroupNames.end()) {
        next.present = true; next.names = found->second.names; next.legacy = found->second.legacy;
    }
    out = std::move(next); return true;
}
bool MatchesDisplayGroupText(const mdl::ModelRecord& model, const mdl::DisplayGroup& group,
                             const DisplayGroupTextState& state) noexcept {
    const auto found = displayGroupNames.find(&group);
    if (!state.present) return found == displayGroupNames.end();
    return found != displayGroupNames.end() && found->second.owner == &model &&
        found->second.names == state.names && found->second.legacy == state.legacy;
}
struct PreparedDisplayGroupTextBatch::Impl {
    struct Entry {
        const mdl::DisplayGroup* group;
        DisplayGroupTextState before;
        DisplayGroupTextMap::node_type after, retired;
    };
    const mdl::ModelRecord* model = nullptr;
    std::vector<Entry> entries;
    std::uint64_t bytes = 0;
    bool consumed = false;
    void Add(std::uint64_t count, std::uint64_t stride) noexcept {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (stride && count > (maximum - bytes) / stride) bytes = maximum;
        else bytes += count * stride;
    }
    template<class Text> void TextBytes(const Text& value) noexcept {
        // Include the terminator and conservatively charge inline text twice,
        // as in the existing retained-resource collector.
        Add(value.capacity(), sizeof(typename Text::value_type)); Add(1, sizeof(typename Text::value_type));
    }
    void StateBytes(const DisplayGroupTextState& state) noexcept {
        for (const auto& value : state.names) TextBytes(value);
        for (const auto& value : state.legacy) TextBytes(value);
    }
    void NodeBytes(const DisplayGroupText& value) noexcept {
        Add(1, sizeof(DisplayGroupTextMap::value_type) + 4 * sizeof(void*));
        for (const auto& text : value.names) TextBytes(text);
        for (const auto& text : value.legacy) TextBytes(text);
    }
};
PreparedDisplayGroupTextBatch::PreparedDisplayGroupTextBatch() noexcept = default;
PreparedDisplayGroupTextBatch::~PreparedDisplayGroupTextBatch() = default;
PreparedDisplayGroupTextBatch::PreparedDisplayGroupTextBatch(PreparedDisplayGroupTextBatch&&) noexcept = default;
PreparedDisplayGroupTextBatch& PreparedDisplayGroupTextBatch::operator=(PreparedDisplayGroupTextBatch&&) noexcept = default;
bool PreparedDisplayGroupTextBatch::Prepare(const mdl::ModelRecord& model,
    const std::vector<std::pair<const mdl::DisplayGroup*, DisplayGroupTextState>>& targets) {
    if (impl_) return false;
    std::vector<const mdl::DisplayGroup*> keys; keys.reserve(targets.size());
    for (const auto& item : targets) { if (!item.first) return false; keys.push_back(item.first); }
    std::sort(keys.begin(), keys.end(), std::less<const mdl::DisplayGroup*>{});
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return false;
    auto next = std::make_unique<Impl>(); next->model = &model; next->entries.reserve(targets.size());
    DisplayGroupTextMap prepared;
    for (const auto& item : targets) {
        Impl::Entry entry{}; entry.group = item.first;
        if (!CaptureDisplayGroupText(model, *item.first, entry.before)) return false;
        if (item.second.present) {
            prepared.emplace(item.first, DisplayGroupText{&model, item.second.names, item.second.legacy});
            entry.after = prepared.extract(item.first);
        }
        next->StateBytes(entry.before);
        // Publication will retain the actual old node outside the live map.
        if (entry.before.present) next->NodeBytes(displayGroupNames.find(item.first)->second);
        if (entry.after) next->NodeBytes(entry.after.mapped());
        next->entries.push_back(std::move(entry));
    }
    next->Add(1, sizeof(Impl)); next->Add(next->entries.capacity(), sizeof(Impl::Entry));
    impl_ = std::move(next); return true;
}
bool PreparedDisplayGroupTextBatch::IsCurrent() const noexcept {
    if (!impl_ || impl_->consumed) return false;
    for (const auto& entry : impl_->entries)
        if (!MatchesDisplayGroupText(*impl_->model, *entry.group, entry.before)) return false;
    return true;
}
void PreparedDisplayGroupTextBatch::Publish() noexcept {
    if (!IsCurrent()) std::terminate();
    try {
        for (auto& entry : impl_->entries) {
            entry.retired = displayGroupNames.extract(entry.group);
            if (entry.after) {
                auto result = displayGroupNames.insert(std::move(entry.after));
                if (!result.inserted) std::terminate();
            }
        }
        impl_->consumed = true;
    } catch (...) { std::terminate(); }
}
std::uint64_t PreparedDisplayGroupTextBatch::RetainedBytes() const noexcept { return impl_ ? impl_->bytes : 0; }

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
