#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace mikumikustudio::plugins {
inline constexpr wchar_t kPropertyInputWindowMarker[] = L"MMS.PluginAbi.PropertyInput.V1";

// A host-owned marker identifies the wrapper and all native field descendants.
// Its value is only a presence token; callers never dereference plugin/UI state.
inline bool OwnsPropertyInput(HWND focus) noexcept {
    for (unsigned depth = 0; focus && depth < 64; ++depth) {
        if (GetPropW(focus, kPropertyInputWindowMarker)) return true;
        const HWND parent = GetParent(focus);
        if (parent == focus) break;
        focus = parent;
    }
    return false;
}
}
