#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "nvidia_antialias_profile.hpp"

namespace mikumikustudio::nvidia_antialias_fix::detail {
// Shared with the elevated helper. No renderer, plug-in, or application runtime
// library may be linked into that DLL.
struct Driver {
    HMODULE module = nullptr;
    Api api;
    bool ready = false;
    int (__cdecl* unload)() = nullptr;
    Driver() {
        module = LoadLibraryExW(sizeof(void*) == 8 ? L"nvapi64.dll" : L"nvapi.dll",
            nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        using Query = void* (__cdecl*)(U32);
        auto query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
        if (!query) return;
        auto init = reinterpret_cast<int (__cdecl*)()>(query(0x0150e828));
        if (!init || init() != 0) return;
        unload = reinterpret_cast<decltype(unload)>(query(0xd22bdd7e));
#define MMS_NVAPI(member, id) api.member = reinterpret_cast<decltype(api.member)>(query(id)); if (!api.member) return
        MMS_NVAPI(createSession, 0x0694d52e);
        MMS_NVAPI(destroySession, 0xdad9cff8);
        MMS_NVAPI(load, 0x375dbd6b);
        MMS_NVAPI(save, 0xfcbc7e14);
        MMS_NVAPI(findProfile, 0x7e4a9a0b);
        MMS_NVAPI(createProfile, 0xcc176068);
        MMS_NVAPI(profileInfo, 0x61cd6fd6);
        MMS_NVAPI(enumApps, 0x7fa2173a);
        MMS_NVAPI(createApp, 0x4347a9de);
        MMS_NVAPI(findApp, 0xeee566b2);
        MMS_NVAPI(get, 0x73bf8338);
        MMS_NVAPI(set, 0x577dd202);
        MMS_NVAPI(remove, 0xe4a26362);
        MMS_NVAPI(globalProfile, 0x617bff9f);
        MMS_NVAPI(baseProfile, 0xda8466a0);
#undef MMS_NVAPI
        ready = true;
    }
    ~Driver() { if (unload) unload(); if (module) FreeLibrary(module); }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
};
inline bool RunForPath(const std::wstring& path, const bool* enabled, State& state) {
    Driver driver;
    if (!driver.ready) { state.error = L"NVIDIA NVAPI is unavailable"; return false; }
    return enabled ? Execute(driver.api, path, enabled, state) : InspectEffective(driver.api, path, state);
}
}
