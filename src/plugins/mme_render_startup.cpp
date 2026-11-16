#include "mikumikustudio/mme_render_startup.hpp"
#include "common.hpp"
#include <d3d9on12.h>
#include <map>

namespace mikumikustudio::plugins {
namespace {
using BridgeSwitch = BOOL (WINAPI*)(BOOL);
BridgeSwitch bridgeSwitch = nullptr;
DWORD startupThread = 0;
std::map<std::string, std::string> startupErrors;
MmsMmeOn12StatusV1 state{Header<MmsMmeOn12StatusV1>(), MMS_MME_BACKEND_D3D9ON12};
}
void SelectMmeRenderBackend(bool preferDx12) noexcept {
    startupThread = GetCurrentThreadId();
    state.requestedBackend = preferDx12 ? MMS_MME_BACKEND_D3D9ON12 : MMS_MME_BACKEND_DX9;
}
void ConfigureMmeRenderStartup(bool enabled, bool loaded, HMODULE bridge) noexcept {
    startupThread = GetCurrentThreadId();
    state.startupRequestEnabled = enabled; state.mmeLoaded = loaded;
    if (state.requestedBackend == MMS_MME_BACKEND_DX9) { state.reason = MMS_MME_ON12_EXPLICIT_DX9; return; }
    if (!enabled) { state.reason = MMS_MME_ON12_NO_REQUEST; return; }
    if (!loaded) { state.reason = MMS_MME_ON12_MME_UNAVAILABLE; return; }
    const auto configure = bridge ? reinterpret_cast<BridgeSwitch>(GetProcAddress(bridge, "MMSSetMmeDx12PatchEnabled")) : nullptr;
    if (!configure || !configure(TRUE)) { state.reason = MMS_MME_ON12_BRIDGE_UNAVAILABLE; return; }
    bridgeSwitch = configure; state.bridgeSupported = 1; state.reason = MMS_MME_ON12_PENDING;
}
bool MmeOn12Attempting() noexcept { return bridgeSwitch != nullptr; }
bool FallBackMmeOn12(uint32_t reason) noexcept {
    if (bridgeSwitch && !bridgeSwitch(FALSE)) { state.reason = MMS_MME_ON12_DISABLE_FAILED; return false; }
    bridgeSwitch = nullptr; state.actualBackend = MMS_MME_BACKEND_UNAVAILABLE; state.reason = reason; return true;
}
bool ConfirmMmeOn12Device(IDirect3DDevice9* device) noexcept {
    if (!bridgeSwitch || !device) return false;
    IDirect3DDevice9On12* interop = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&interop))) || !interop) return false;
    ID3D12Device* gpu = nullptr;
    const auto result = interop->GetD3D12Device(IID_PPV_ARGS(&gpu));
    interop->Release();
    if (FAILED(result) || !gpu) return false;
    gpu->Release(); state.actualBackend = MMS_MME_BACKEND_D3D9ON12; state.reason = MMS_MME_ON12_ACTIVE; return true;
}
void RecordMmeDx9Device() noexcept {
    if (!bridgeSwitch) state.actualBackend = state.mmeLoaded ? MMS_MME_BACKEND_DX9 : MMS_MME_BACKEND_UNAVAILABLE;
}
MmsMmeOn12StatusV1 MmeRenderStatus() noexcept { return state; }
const char* MmeRenderReason() noexcept {
    switch (state.reason) {
    case MMS_MME_ON12_ACTIVE: return "D3D12 active via D3D9On12 (shared host/MME device)";
    case MMS_MME_ON12_EXPLICIT_DX9: return "DX9 selected explicitly";
    case MMS_MME_ON12_NO_REQUEST: return "DX9: no enabled mme-d3d9on12 startup request";
    case MMS_MME_ON12_MME_UNAVAILABLE: return "DX9: original MME did not load";
    case MMS_MME_ON12_BRIDGE_UNAVAILABLE: return "DX9 fallback: loaded MME bridge does not support D3D9On12";
    case MMS_MME_ON12_FACTORY_FAILED: return "DX9 fallback: D3D9On12 factory creation failed";
    case MMS_MME_ON12_DEVICE_FAILED: return "DX9 fallback: D3D9On12 device creation failed";
    case MMS_MME_ON12_CONFIRMATION_FAILED: return "DX9 fallback: device did not expose D3D9On12";
    case MMS_MME_ON12_DISABLE_FAILED: return "startup aborted: bridge could not restore DX9";
    default: return "D3D9On12 pending device confirmation";
    }
}
void RecordMmeStartupError(const std::string& id, const std::string& message) { startupErrors[id] = message; }
std::string MmeStartupErrors() {
    std::string text;
    for (const auto& error : startupErrors) { if (text.size() > 12000) break; text += error.first + ": " + error.second + "\n\n"; }
    return text;
}
MmsMmeOn12ApiV1 MmeOn12Api() noexcept {
    MmsMmeOn12ApiV1 api{Header<MmsMmeOn12ApiV1>(), nullptr};
    api.query = [](void*, MmsMmeOn12StatusV1* out) -> MmsResult {
        if (!Valid(out)) return MMS_E_ARGUMENT;
        if (startupThread && GetCurrentThreadId() != startupThread) return MMS_E_THREAD;
        *out = state; return MMS_OK;
    };
    return api;
}
}
