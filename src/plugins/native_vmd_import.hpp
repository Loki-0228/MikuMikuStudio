#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// VMD still uses legacy writers. Until all participating pools and extension
// authoring can be prepared privately, it must not bypass a bound journal.
// MMS_OK means only that the legacy, unbound route may continue.
MmsResult CheckVmdImportHistory(MMDApp*) noexcept;
// Displays the migration limitation outside any journal reservation. True
// consumes the operation before opening a file or changing live scene state.
bool RefuseBoundVmdImport(MMDApp*) noexcept;
}
#endif
