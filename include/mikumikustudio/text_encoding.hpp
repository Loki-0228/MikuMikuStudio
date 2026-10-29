#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <array>

namespace mikumikustudio::mdl { struct ModelRecord; struct MorphRecord; struct BoneRecord; struct DisplayGroup; }
namespace mikumikustudio::text_encoding {
// PMM has fixed byte fields and no declared code page. Keep legacy encodings
// when exact; use a UTF-8 BOM only for text no legacy code page can represent.
bool Decode(const char* bytes, unsigned int codePage, std::wstring& result);
bool EncodeExact(const wchar_t* text, unsigned int codePage, std::string& result);
// Fixed PMX identity mirror; full Unicode names stay in model metadata.
void EncodePmxName(char* destination, std::size_t capacity, const wchar_t* text);
// Only use at the same index when the saved and loaded table sizes agree.
bool MatchesOldPmxName(const char* saved, const wchar_t* text, std::size_t capacity);
bool EncodePath(char* destination, std::size_t capacity, const wchar_t* path);
bool EncodeDisplay(char* destination, std::size_t capacity, const wchar_t* text);
std::wstring Display(const char* text);
std::wstring LegacyFilename(const char* text, const wchar_t* resolvedPath);
bool HasUtf8Bom(const char* text);
std::wstring ModelName(const mdl::ModelRecord& model, bool english);
using MaterialNames = std::vector<std::array<std::wstring,2>>;
void SetMaterialNames(const mdl::ModelRecord&,MaterialNames);
void ForgetMaterialNames(const mdl::ModelRecord&);
std::wstring MaterialName(const mdl::ModelRecord&,std::size_t index,bool english);
// Prefer model metadata in the selected language; use the source filename only
// when neither name field contains a name.
std::wstring ModelLabel(const mdl::ModelRecord& model, bool english);
std::wstring MorphName(const mdl::MorphRecord& morph, bool english);
std::wstring BoneName(const mdl::BoneRecord& bone, bool english);
// Preserve PMX group names outside the fixed legacy display-group layout.
void SetDisplayGroupName(const mdl::ModelRecord& model, const mdl::DisplayGroup& group,
                         const wchar_t* native, const wchar_t* english);
void ForgetDisplayGroupNames(const mdl::ModelRecord& model);
std::wstring DisplayGroupName(const mdl::DisplayGroup& group, bool english);
// Display-only dictionary translation. Raw name accessors above remain suitable
// for file formats, motion matching, and plugin identities.
std::wstring BoneLabel(const mdl::BoneRecord& bone, bool english);
std::wstring MorphLabel(const mdl::MorphRecord& morph, bool english);
std::wstring DisplayGroupLabel(const mdl::DisplayGroup& group, bool english);
std::wstring MaterialLabel(const mdl::ModelRecord& model, std::size_t index, bool english);
// Message boxes whose text comes from the byte-exact Shift-JIS literals of the
// original image (the JP branches) or from plain ASCII (the EN branches).
// MessageBoxA would decode those bytes with the *system* code page and garble
// every Japanese line on a non-Japanese Windows, so decode as CP932 and call
// MessageBoxW.  owner is an HWND (kept as void* so this header stays
// Windows-free); the return value matches MessageBoxW.
int MessageBoxJp(void* owner, const char* body, const char* caption,
                 unsigned int flags);
}
