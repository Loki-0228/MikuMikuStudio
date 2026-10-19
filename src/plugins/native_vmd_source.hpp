#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_global_paste.hpp"
#include "mikumikustudio/accessory_motion_io.hpp"
#include <string>
namespace mikumikustudio::plugins {
// Decodes one immutable file snapshot. Standard record values are owned by
// source. Valid accessory extensions remain owned until scene matching and
// private planning are complete; unknown or damaged tails remain ignorable.
struct GlobalVmdSource {
    GlobalPasteSource source;
    std::vector<accessory_motion_io::Track> accessoryTracks;
    int version=0;
    std::string modelName;
};
MmsResult DecodeGlobalVmdSource(const char* ansiPath,const wchar_t* widePath,
    std::uint32_t frameOffset,GlobalVmdSource&) noexcept;
MmsResult PlanGlobalVmdImport(MMDApp&,const char* ansiPath,const wchar_t* widePath,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
