#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

// Keep a real GDI+ import without starting its runtime from DllMain.
extern "C" __declspec(dllexport) int __cdecl version() {
    auto* memory = Gdiplus::DllExports::GdipAlloc(1);
    Gdiplus::DllExports::GdipFree(memory);
    return 3;
}
extern "C" __declspec(dllexport) void* __cdecl create3(void*) { return nullptr; }
extern "C" __declspec(dllexport) void __cdecl destroy3(void*) {}
