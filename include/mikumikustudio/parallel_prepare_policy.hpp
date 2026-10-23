#pragma once
#include <Windows.h>

namespace mikumikustudio::parallel_prepare_policy {
inline bool Disabled() noexcept {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"MMS_PARALLEL_PREPARE_DISABLE", value, 8) == 1
        && value[0] == L'1';
}
}
