#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// MMS_OK means the unbound legacy route may continue. A bound journal always
// owns this entry: standard model/global and accessory VMD use private plans.
MmsResult CheckVmdImportHistory(MMDApp*) noexcept;
// Compatibility helper: model imports now use TryImportBoundVmd. Returns false.
bool RefuseBoundVmdImport(MMDApp*) noexcept;
// Owns bound imports including rejection/no-op. Never falls through to the
// legacy loader after a private plan or transaction has failed. Return value
// follows the existing VMD loader convention (zero even on global success).
bool TryImportBoundVmd(MMDApp*,const char* ansiPath,const wchar_t* widePath,int& result) noexcept;
}
#endif
