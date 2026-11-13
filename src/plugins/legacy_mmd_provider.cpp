#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" __declspec(dllexport) HWND __cdecl getHWND() {
    using GetWindow = HWND(__cdecl*)();
    const auto getWindow = reinterpret_cast<GetWindow>(
        GetProcAddress(GetModuleHandleW(nullptr), "MMSLegacyMmdGetWindow"));
    return getWindow ? getWindow() : nullptr;
}

extern "C" __declspec(dllexport) int __cdecl createWM_APP_ID() {
    using Allocate = int(__cdecl*)();
    const auto allocate = reinterpret_cast<Allocate>(
        GetProcAddress(GetModuleHandleW(nullptr), "MMSLegacyMmdAllocateMenuId"));
    return allocate ? allocate() : -1;
}
