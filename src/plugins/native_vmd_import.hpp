#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// MMS_OK means the unbound legacy route may continue. A bound journal always
// owns this entry: global standard VMD uses a private plan, model VMD and valid
// nonempty accessory extensions remain unavailable under that binding.
MmsResult CheckVmdImportHistory(MMDApp*) noexcept;
// Model-only migration limitation, outside any journal reservation. True
// consumes the operation before opening a file or changing live scene state.
bool RefuseBoundVmdImport(MMDApp*) noexcept;
// Owns bound imports including rejection/no-op. Never falls through to the
// legacy loader after a private plan or transaction has failed. Return value
// follows the existing VMD loader convention (zero even on global success).
bool TryImportBoundVmd(MMDApp*,const char* ansiPath,const wchar_t* widePath,int& result) noexcept;
}
#endif
