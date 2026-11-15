#include <Windows.h>
namespace { unsigned enabledCalls = 0, disabledCalls = 0; }
extern "C" __declspec(dllexport) BOOL WINAPI MMSSetMmeDx12PatchEnabled(BOOL enabled) {
    if (enabled) ++enabledCalls; else ++disabledCalls;
    return !GetEnvironmentVariableW(enabled ? L"MMS_TEST_DENY_ON12" : L"MMS_TEST_DENY_DX9", nullptr, 0);
}
extern "C" __declspec(dllexport) unsigned WINAPI MMSFixtureCalls(BOOL enabled) { return enabled ? enabledCalls : disabledCalls; }
