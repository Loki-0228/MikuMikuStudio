#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace mikumikustudio::cache_storage_policy {
// Optional diagnostic mode for regenerable effect/texture preparation caches.
// Live renderer caches already stay in RAM/VRAM; settings and backups are
// independent. Read dynamically so isolated tests need no saved preference.
inline bool MemoryOnly() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", value, 2) == 1 && value[0] == L'1';
}
}
