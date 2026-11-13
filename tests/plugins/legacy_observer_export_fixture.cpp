#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        SetEnvironmentVariableW(L"MMS_TEST_LEGACY_OBSERVER_ATTACHED", L"1");
    return TRUE;
}

extern "C" __declspec(dllexport) int version() { return 3; }
extern "C" __declspec(dllexport) void* create3(void*) { return nullptr; }

#ifdef MMS_TEST_LEGACY_DIRECT_DESTROY
extern "C" __declspec(dllexport) void destroy3(void*) {}
#endif
