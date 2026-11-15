#include "mikumikustudio/mme_render_startup.hpp"
#include "common.hpp"
#include <d3d9on12.h>
#include <iostream>
#include <thread>
using namespace mikumikustudio::plugins;
namespace {
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3, "expected fixture and scenario");
        const std::wstring mode = argv[2];
        const auto bridge = LoadLibraryW(argv[1]); Check(bridge != nullptr, "test bridge load");
        const auto calls = reinterpret_cast<unsigned(WINAPI*)(BOOL)>(GetProcAddress(bridge, "MMSFixtureCalls"));
        Check(calls != nullptr, "fixture counters");
        SelectMmeRenderBackend(mode != L"dx9");
        if (mode == L"deny") SetEnvironmentVariableW(L"MMS_TEST_DENY_ON12", L"1");
        ConfigureMmeRenderStartup(mode != L"disabled", mode != L"missing", mode == L"bridge-missing" ? nullptr : bridge);
        auto api = MmeOn12Api();
        MmsMmeOn12StatusV1 status{Header<MmsMmeOn12StatusV1>()};
        Check(api.query(api.context, &status) == MMS_OK, "read-only status service");
        Check(api.query(api.context, nullptr) == MMS_E_ARGUMENT, "status validates output");
        MmsResult threadResult = MMS_OK;
        std::thread worker([&] { threadResult = api.query(api.context, &status); }); worker.join();
        Check(threadResult == MMS_E_THREAD, "status enforces UI thread");
        if (mode == L"dx9" || mode == L"disabled" || mode == L"missing" || mode == L"bridge-missing") {
            Check(calls(TRUE) == 0 && !MmeOn12Attempting(), "ineligible request must not call bridge");
            const auto expected = mode == L"dx9" ? MMS_MME_ON12_EXPLICIT_DX9 : mode == L"disabled" ? MMS_MME_ON12_NO_REQUEST :
                mode == L"missing" ? MMS_MME_ON12_MME_UNAVAILABLE : MMS_MME_ON12_BRIDGE_UNAVAILABLE;
            Check(status.reason == expected, "ineligible request reason");
        } else if (mode == L"deny") {
            Check(!MmeOn12Attempting() && status.reason == MMS_MME_ON12_BRIDGE_UNAVAILABLE, "unavailable bridge falls back");
        } else {
            Check(MmeOn12Attempting() && calls(TRUE) == 1 && status.actualBackend == MMS_MME_BACKEND_UNAVAILABLE,
                "request is not proof of actual backend");
            Check(!ConfirmMmeOn12Device(nullptr), "missing device cannot confirm DX12");
            if (mode == L"hardware") {
                const auto d3dModule = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                const auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(d3dModule, "Direct3DCreate9On12"));
                Check(create != nullptr, "system D3D9On12 export");
                D3D9ON12_ARGS args{}; args.Enable9On12 = TRUE;
                auto* d3d = create(D3D_SDK_VERSION, &args, 1); Check(d3d != nullptr, "real On12 factory");
                HWND window = CreateWindowW(L"STATIC", L"MMS backend check", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                D3DPRESENT_PARAMETERS parameters{}; parameters.Windowed = TRUE; parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
                parameters.hDeviceWindow = window; parameters.BackBufferWidth = parameters.BackBufferHeight = 64;
                IDirect3DDevice9* device = nullptr;
                Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &parameters, &device)), "real On12 device");
                Check(ConfirmMmeOn12Device(device) && MmeRenderStatus().actualBackend == MMS_MME_BACKEND_D3D9ON12,
                    "confirm actual IDirect3DDevice9On12 and ID3D12Device");
                device->Release(); d3d->Release(); DestroyWindow(window); FreeLibrary(d3dModule);
            }
            if (mode == L"disable-failed") {
                SetEnvironmentVariableW(L"MMS_TEST_DENY_DX9", L"1");
                Check(!FallBackMmeOn12(MMS_MME_ON12_DEVICE_FAILED) && MmeRenderStatus().reason == MMS_MME_ON12_DISABLE_FAILED,
                    "failed switch must abort rather than falsely report DX9");
            } else {
                Check(FallBackMmeOn12(MMS_MME_ON12_DEVICE_FAILED) && calls(FALSE) == 1 && !MmeOn12Attempting(), "one-way startup fallback");
                RecordMmeDx9Device();
                Check(MmeRenderStatus().actualBackend == MMS_MME_BACKEND_DX9, "record actual fallback device");
            }
        }
        FreeLibrary(bridge); std::cout << "PASS MME startup protocol\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
