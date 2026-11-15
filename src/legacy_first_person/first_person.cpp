// Adapted from MikuMikuShaders/FirstPersonCamera f71c300 (MIT; see LICENSE).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dx9.h>
#include <DirectXMath.h>
#include <mms/legacy_camera.h>
#include "../../third_party/mmdplugin/callbacks.hpp"

namespace {
class FirstPersonCamera final : public MMDPluginDLL4 {
    const MmsLegacyCameraApiV1* api_;
    bool locked_ = false;
    POINT last_{};
    bool Read(MmsLegacyCameraStateV1& state) {
        state = {}; state.byteSize = sizeof(state); state.version = MMS_LEGACY_CAMERA_API_VERSION;
        if (api_->read(&state) != MMS_OK || !(state.flags & MMS_LEGACY_CAMERA_EDITABLE)) {
            Unlock(); return false;
        }
        return true;
    }
    void Unlock() noexcept {
        if (locked_) { ShowCursor(TRUE); locked_ = false; }
    }
    static D3DXVECTOR3 Direction(const MmsLegacyCameraStateV1& state, const D3DXVECTOR3& axis) {
        const auto rotate = DirectX::XMMatrixRotationRollPitchYaw(-state.rotation[0], -state.rotation[1], state.rotation[2]);
        DirectX::XMFLOAT3 input(axis.x, axis.y, axis.z), result;
        DirectX::XMStoreFloat3(&result, DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&input), rotate));
        return D3DXVECTOR3(result.x, result.y, result.z);
    }
public:
    explicit FirstPersonCamera(const MmsLegacyCameraApiV1* api) : api_(api) {}
    ~FirstPersonCamera() override { Unlock(); }
    const char* getPluginTitle() const override { return "FirstPersonCamera (MMS)"; }
    void stop() override { Unlock(); }
    void KeyBoardProc(WPARAM key, LPARAM bits) override {
        if (bits & (LPARAM(1) << 31)) return; // Release never moves the camera.
        if (key == VK_ESCAPE) { Unlock(); return; }
        if (key != 'W' && key != 'S' && key != 'A' && key != 'D') return;
        MmsLegacyCameraStateV1 before;
        if (!Read(before)) return;
        auto after = before;
        const float speed = (GetKeyState(VK_SHIFT) & 0x8000) ? 4.f : 1.f;
        const bool sideways = key == 'A' || key == 'D';
        auto direction = Direction(before, sideways ? D3DXVECTOR3(1,0,0) : D3DXVECTOR3(0,0,-1));
        const float amount = sideways ? (key == 'A' ? -.5f : .5f) * speed : (key == 'W' ? -speed : speed);
        after.position[0] += direction.x * amount;
        after.position[1] += direction.y * amount;
        after.position[2] += direction.z * amount;
        if (api_->edit(&before, &after) != MMS_OK) Unlock();
    }
    void MouseProc(WPARAM message, MOUSEHOOKSTRUCT* mouse) override {
        if (message == WM_LBUTTONUP || message == WM_CANCELMODE || message == WM_KILLFOCUS) { Unlock(); return; }
        MmsLegacyCameraStateV1 before;
        if (!mouse || !Read(before)) return;
        if (message == WM_LBUTTONDOWN) {
            if (!locked_) { last_ = mouse->pt; ShowCursor(FALSE); locked_ = true; }
        } else if (message == WM_MOUSEMOVE && locked_) {
            const auto dx = mouse->pt.x - last_.x, dy = mouse->pt.y - last_.y;
            last_ = mouse->pt;
            if (!dx && !dy) return;
            auto after = before;
            const auto forward = Direction(before, D3DXVECTOR3(0,0,1));
            after.rotation[1] -= dx / 500.f;
            after.rotation[0] -= dy / 500.f;
            after.position[0] += forward.x * before.distance;
            after.position[1] += forward.y * before.distance;
            after.position[2] += forward.z * before.distance;
            after.distance = 0;
            if (api_->edit(&before, &after) != MMS_OK) Unlock();
        }
    }
};
}
extern "C" __declspec(dllexport) int __cdecl version() { return 4; }
extern "C" __declspec(dllexport) unsigned __cdecl MMSLegacyCameraAdapterVersion() { return MMS_LEGACY_CAMERA_API_VERSION; }
extern "C" __declspec(dllexport) MMDPluginDLL4* __cdecl create4(IDirect3DDevice9*) {
    const auto acquire = reinterpret_cast<MmsAcquireLegacyCameraApiFn>(GetProcAddress(GetModuleHandleW(nullptr), MMS_LEGACY_CAMERA_API_EXPORT));
    const auto* api = acquire ? acquire(MMS_LEGACY_CAMERA_API_VERSION) : nullptr;
    if (!api || api->byteSize < sizeof(*api) || api->version != MMS_LEGACY_CAMERA_API_VERSION || !api->read || !api->edit) return nullptr;
    return new FirstPersonCamera(api);
}
extern "C" __declspec(dllexport) void __cdecl destroy4(MMDPluginDLL4* camera) { delete camera; }
