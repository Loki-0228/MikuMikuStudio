#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <memory>
#include <mms/plugin.h>
namespace mikumikustudio {class MMDApp;}
namespace mikumikustudio::plugins {
class SharedHistory;
// A modal input snapshot, without an edit reservation or scene ownership.
class VpdImportSession {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    VpdImportSession();~VpdImportSession();
    MmsResult Begin(MMDApp&,SharedHistory&) noexcept;
    MmsResult Apply(const wchar_t* path,bool& changed,bool resetPhysics=false) noexcept;
};
MmsResult ImportBoundVpd(MMDApp&,const wchar_t* path,bool& changed,bool resetPhysics=false) noexcept;
bool TryLoadBoundVpd(MMDApp*,const wchar_t* path,bool rememberDirectory=false) noexcept;
bool TryPickBoundVpd(MMDApp*) noexcept;
}
#endif
