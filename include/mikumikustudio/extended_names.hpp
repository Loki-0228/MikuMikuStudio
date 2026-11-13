#pragma once
#include "mikumikustudio/pmm_extension.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <cstdio>
#include <optional>
namespace mikumikustudio { class MMDApp; namespace mdl { struct ModelRecord; }
namespace extended_names {
// Replaces an owned Unicode string only after its replacement is allocated.
bool Assign(wchar_t*& destination, const std::wstring& value) noexcept;
void CapturePmm(const MMDApp&, pmm_extension::Document&);
// Names v1 and the independent comments v1 records are fully parsed and all
// replacements prepared across models before publishing any live field.
// Comment payload: u32 version=1, u32 presence (bit0 local, bit1 English),
// then length-prefixed UTF-8 strings for the set bits, in language order.
// Invalid raw text without Unicode, or registered Unicode with a nonzero
// allocation tail after its first NUL, is omitted independently per language;
// missing records/bits leave the corresponding source-model comment intact.
// An adapted model keeps its source names: the old positional dictionary
// describes another table even if its counts still agree. Comments are an
// independent record and keep their ordinary loading/atomicity rules.
void LoadPmm(MMDApp&, const pmm_extension::Document&,
             const std::vector<int>& adaptedModelSlots = {});
bool LegacyNamesFit(const mdl::ModelRecord&, bool motion);
// Exact fidelity of the fields actually written by the standard PMD writer.
// Each language is checked independently, without display-name fallbacks.
// Full-width legacy fields are valid; Unicode with a stale raw mirror is not.
bool CanSavePmdNames(const mdl::ModelRecord&);
struct MotionNames {
    std::wstring model;
    std::vector<std::wstring> bones, morphs;
    std::vector<std::uint32_t> boneOwners, morphOwners, ikOwners;
    std::optional<std::array<std::uint8_t, 32>> boneRig;
    std::optional<std::array<std::uint8_t, 32>> morphTable;
};
struct MotionTargets {
    std::vector<int> bones, morphs;
};
// Standard records retain their names. The extension binds owner indices to
// matching ordered bone and morph definitions when names are ambiguous.
bool PrepareMotionNames(const mdl::ModelRecord&, bool hasMorphKeys, MotionNames&);
MotionTargets ResolveMotionTargets(const MotionNames&, const mdl::ModelRecord&);
inline thread_local const std::vector<int>* importedIkTargets = nullptr;
inline thread_local bool exportingUnicodeMotion = false;
inline thread_local const std::vector<int>* importedBoneTargets = nullptr;
inline thread_local const std::vector<int>* importedMorphTargets = nullptr;
struct MotionScope {
    bool previousExport = exportingUnicodeMotion;
    const std::vector<int>* previousBones = importedBoneTargets;
    const std::vector<int>* previousMorphs = importedMorphTargets;
    explicit MotionScope(bool exporting, const std::vector<int>* bones=nullptr,
                         const std::vector<int>* morphs=nullptr) {
        exportingUnicodeMotion=exporting; importedBoneTargets=bones; importedMorphTargets=morphs;
    }
    ~MotionScope() {
        exportingUnicodeMotion=previousExport; importedBoneTargets=previousBones; importedMorphTargets=previousMorphs;
    }
};
// Internal extension identity only. The standard VMD keeps its original names;
// the checksum-bound Unicode dictionary resolves this source index on import.
inline std::array<char,15> MotionIndexName(std::uint32_t index) {
    std::array<char,15> result{};
    std::snprintf(result.data(),result.size(),"MMS#%08x",index);
    return result;
}
std::vector<std::uint8_t> EncodeMotion(const MotionNames&, std::uint64_t prefixSize, std::uint32_t prefixCrc);
// Returns the consumed block size. Unknown blocks return zero; malformed known
// blocks throw before any keys are imported.
std::size_t DecodeMotion(const std::uint8_t* file, std::size_t size, std::size_t offset, MotionNames&);
}}
