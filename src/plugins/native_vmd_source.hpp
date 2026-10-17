#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_paste.hpp"
#include <string>
namespace mikudancestudio::plugins {
// Decodes one immutable file snapshot. Standard record values are owned by
// source. A valid nonempty accessory extension is refused until privately
// planned; it must never be silently dropped from a partial import.
struct GlobalVmdSource {
    GlobalPasteSource source;
    int version=0;
    std::string modelName;
};
MmsResult DecodeGlobalVmdSource(const char* ansiPath,const wchar_t* widePath,
    std::uint32_t frameOffset,GlobalVmdSource&) noexcept;
MmsResult PlanGlobalVmdImport(MMDApp&,const char* ansiPath,const wchar_t* widePath,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
