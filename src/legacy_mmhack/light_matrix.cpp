#include "light_matrix.hpp"
#include <d3dx9.h>
#include <cstring>
namespace mme_re::mmhack {
HRESULT light_view_projection(State& state, const HostApi& host, D3DMATRIX& output_view, D3DMATRIX& output_projection, D3DMATRIX& output_combined) {
  if (!state.effect) return E_UNEXPECTED;
  D3DXMATRIX combined;
  auto result = state.effect->GetMatrix("matLightViewProj", &combined);
  if (FAILED(result)) return result;
  if (state.draw_type == 5 && state.current_model_kind == ModelKind::accessory) {
    if (!host.current_object || !host.accessory_count || !host.accessory_order || !host.accessory_world) return E_UNEXPECTED;
    const int object = host.current_object();
    const int count = host.accessory_count();
    for (int i = 0; i < count; ++i) {
      if (host.accessory_order(i) != object) continue;
      D3DXMATRIX world;
      host.accessory_world(&world._11, i);
      D3DXMATRIX inverse;
      if (!D3DXMatrixInverse(&inverse, nullptr, &world)) return D3DERR_INVALIDCALL;
      combined = inverse * combined;
      break;
    }
  }
  IDirect3DDevice9* device = nullptr;
  result = state.effect->GetDevice(&device);
  if (FAILED(result)) return result;
  if (!device) return E_UNEXPECTED;
  D3DLIGHT9 light{};
  D3DXMATRIX camera_view;
  result = device->GetLight(0, &light); // 0x1a0 is slot 52, GetLight
  if (SUCCEEDED(result)) result = device->GetTransform(D3DTS_VIEW, &camera_view);
  device->Release();
  if (FAILED(result)) return result;
  D3DXMATRIX camera_inverse;
  if (!D3DXMatrixInverse(&camera_inverse, nullptr, &camera_view)) return D3DERR_INVALIDCALL;
  const D3DXVECTOR3 target(camera_inverse._41, camera_inverse._42, camera_inverse._43);
  const D3DXVECTOR3 eye(target.x - light.Direction.x * 50.0f, target.y - light.Direction.y * 50.0f, target.z - light.Direction.z * 50.0f);
  const D3DXVECTOR3 up(0, 0, 1);
  D3DXMATRIX look_at;
  D3DXMatrixLookAtLH(&look_at, &eye, &target, &up);
  const D3DXMATRIX world(state.world_matrix);
  const D3DXMATRIX view = world * look_at;
  D3DXMATRIX view_inverse;
  if (!D3DXMatrixInverse(&view_inverse, nullptr, &view)) return D3DERR_INVALIDCALL;
  output_combined = combined;
  output_projection = view_inverse * combined;
  output_view = view;
  return S_OK;
}
namespace {
BOOL CALLBACK find_recording_window(HWND window, LPARAM parameter) {
  char name[32]{};
  if (GetClassNameA(window, name, sizeof(name)) && std::strcmp(name, "RecWindow") == 0) {
    *reinterpret_cast<bool*>(parameter) = true;
    return FALSE;
  }
  return TRUE;
}
}
bool detect_playback(State& state) {
  if (!state.frame_edit) {
    for (const int control : {0x13d, 0x1a1}) {
      HWND candidate = GetDlgItem(state.main_window, control);
      char name[32]{};
      if (candidate && GetClassNameA(candidate, name, sizeof(name)) && _stricmp(name, "EDIT") == 0) { state.frame_edit = candidate; break; }
    }
  }
  bool recording = false;
  EnumThreadWindows(GetCurrentThreadId(), &find_recording_window, reinterpret_cast<LPARAM>(&recording));
  if (!recording) state.recording_modal = false;
  if (!IsWindowEnabled(state.frame_edit)) return true;
  if (recording) {
    if (state.recording_modal) return true;
    state.recording_modal = true;
  }
  return false;
}
}
