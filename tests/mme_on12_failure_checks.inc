#include <d3d9on12.h>
namespace mme_on12_failure_checks {
using namespace mikumikustudio;
PFN_Direct3DCreate9On12 originalFactory = nullptr;
using DeviceFactory = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
DeviceFactory originalDevice = nullptr;
bool failFactory = false;
HRESULT WINAPI CreateDevice(IDirect3D9* factory, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
    D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** out) {
    if (plugins::MmeOn12Attempting()) { *out = nullptr; std::puts("INJECT On12 CreateDevice failure"); return E_FAIL; }
    return originalDevice(factory, adapter, type, window, flags, parameters, out);
}
IDirect3D9* WINAPI CreateFactory(UINT sdk, D3D9ON12_ARGS* overrides, UINT count) {
    if (failFactory) { std::puts("INJECT On12 factory failure"); return nullptr; }
    auto* factory = originalFactory(sdk, overrides, count);
    if (factory && !originalDevice) {
        auto* address = (*reinterpret_cast<void***>(factory))[16];
        Check(MH_CreateHook(address, reinterpret_cast<void*>(&CreateDevice), reinterpret_cast<void**>(&originalDevice)) == MH_OK,
            "install isolated On12 device failure hook");
        Check(MH_EnableHook(address) == MH_OK, "enable isolated device failure hook");
    }
    return factory;
}
void Install() {
    wchar_t mode[32]{};
    if (!GetEnvironmentVariableW(L"MMDFACADE_TEST_ON12_FAILURE", mode, 32)) return;
    failFactory = std::wcscmp(mode, L"factory") == 0;
    const auto initialized = MH_Initialize();
    Check(initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED, "failure hook initialization");
    auto* address = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"d3d9.dll"), "Direct3DCreate9On12"));
    Check(address && MH_CreateHook(address, reinterpret_cast<void*>(&CreateFactory), reinterpret_cast<void**>(&originalFactory)) == MH_OK,
        "install isolated On12 factory failure hook");
    Check(MH_EnableHook(address) == MH_OK, "enable isolated factory failure hook");
}
}
