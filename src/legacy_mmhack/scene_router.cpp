#include "scene_router.hpp"
#include "light_matrix.hpp"
#include "material_parser.hpp"
#include "technique_table.hpp"
#include <d3dx9.h>
#include <array>
#include <unordered_set>
namespace mme_re::mmhack {
SceneRouter::SceneRouter(IDirect3DDevice9* device, State& value, const Callbacks& callbacks) noexcept : device_(device), state_(value), callbacks_(callbacks) {}
HRESULT SceneRouter::initialize() {
  if (initialized_) return S_OK;
  if (!device_ || !callbacks_.initialize) return E_UNEXPECTED;
  if (callbacks_.initialize(device_) != 0) {
    if (state_.main_window) {
      const char* message = state_.effect ? "Initialize Error" : "Initialize Error: failed to load default effect file. Please check video card capability.";
      MessageBoxA(state_.main_window, message, "MikuMikuEffect", MB_ICONERROR);
    }
    state_.disabled = true;
    return E_FAIL;
  }
  initialized_ = true;
  return S_OK;
}
HRESULT SceneRouter::begin_scene(const HostApi& host) {
  ended_ = false;
  if (!host.frame_time || !host.pmd_count || !host.accessory_count || !host.pmd_id || !host.accessory_id || !host.pmd_filename || !host.accessory_filename || !host.pmd_order || !host.accessory_order || !host.pmd_material_count || !host.accessory_material_count) return E_UNEXPECTED;
  state_.playing = detect_playback(state_);
  state_.frame_time = host.frame_time();
  state_.active_models = &models_;
  const auto initialized = initialize();
  // 0x2de0 marks effects disabled and returns zero before calling the device.
  if (FAILED(initialized)) return S_OK;
  const auto result = device_->BeginScene();
  if (result != S_OK) return result;
  const int pmd_count = host.pmd_count();
  const int accessory_count = host.accessory_count();
  // Refresh the full native pointer and wide path each frame, before any
  // resource discovery. Exp IDs remain the legacy unsigned 32-bit values.
  std::unordered_map<ModelId, IUnknown*> host_resources;
  if (host.object_backing) {
    state_.object_backings.clear();
    for (int kind = 0; kind < 2; ++kind) {
      const int count = kind ? pmd_count : accessory_count;
      for (int i = 0; i < count; ++i) {
        const auto id = kind ? host.pmd_id(i) : host.accessory_id(i);
        const wchar_t* path = nullptr; IUnknown* resource = nullptr;
        auto* base = host.object_backing(kind, i, &path, &resource);
        if (base) state_.object_backings[id] = {static_cast<std::byte*>(base), path ? path : L""};
        host_resources[id] = resource;
      }
    }
  }
  std::unordered_set<ModelId> present;
  for (int i = 0; i < pmd_count; ++i) present.insert(host.pmd_id(i));
  for (int i = 0; i < accessory_count; ++i) present.insert(host.accessory_id(i));
  std::vector<ModelRecord> removed_models;
  for (auto iterator = models_.begin(); iterator != models_.end();) {
    auto& model = iterator->second;
    bool remove = present.find(iterator->first) == present.end();
    if (!remove && model.retained_resource) {
      // The original probes the retained COM reference using Release/AddRef.
      const auto count = model.retained_resource->Release();
      if (count == 0) { model.retained_resource = nullptr; remove = true; }
      else model.retained_resource->AddRef();
    }
    if (!remove) { ++iterator; continue; }
    removed_models.emplace_back(std::move(model));
    iterator = models_.erase(iterator);
  }
  // The original removes all records first and dispatches deletions from a
  // separate vector; getters cannot see a model inside OnDeleteModel.
  for (auto& model : removed_models) {
    if (callbacks_.delete_model) callbacks_.delete_model(device_, model.id);
    if (model.retained_resource) model.retained_resource->Release();
  }
  auto add_models = [&](ModelKind kind, int count) {
    for (int i = 0; i < count; ++i) {
      const ModelId id = kind == ModelKind::pmd ? host.pmd_id(i) : host.accessory_id(i);
      if (models_.find(id) != models_.end() || !callbacks_.create_model) continue;
      IUnknown* resource = host.object_backing ? host_resources[id] : state_.discover_resource(id, kind);
      if (!resource) continue;
      const auto* filename = kind == ModelKind::pmd ? host.pmd_filename(i) : host.accessory_filename(i);
      const int materials = kind == ModelKind::pmd ? host.pmd_material_count(i) : host.accessory_material_count(i);
      ModelRecord model;
      model.id = id; model.kind = kind;
      if (filename) model.filename = filename;
      model.retained_resource = resource;
      resource->AddRef();
      if (kind == ModelKind::pmd) {
        MaterialTable table;
        std::string error;
        const auto backing = state_.object_backing(id);
        if (!backing.filename.empty() && parse_material_table(backing.filename, table, error)) model.materials = std::move(table.materials);
      }
      auto inserted = models_.emplace(id, std::move(model));
      callbacks_.create_model(device_, id, inserted.first->second.filename.c_str(), static_cast<std::uint32_t>(kind), static_cast<std::uint32_t>(materials), 0, nullptr);
    }
  };
  add_models(ModelKind::pmd, pmd_count);
  add_models(ModelKind::accessory, accessory_count);
  draw_orders_.clear();
  for (int i = 0; i < pmd_count; ++i) {
    const ModelId id = host.pmd_id(i);
    if (models_.find(id) != models_.end()) draw_orders_[host.pmd_order(i)] = id;
  }
  for (int i = 0; i < accessory_count; ++i) {
    const ModelId id = host.accessory_id(i);
    if (models_.find(id) != models_.end()) draw_orders_[host.accessory_order(i)] = id;
  }
  device_->GetTransform(D3DTS_WORLD, &state_.world_matrix);
  D3DXMatrixInverse(reinterpret_cast<D3DXMATRIX*>(&state_.world_inverse), nullptr, reinterpret_cast<const D3DXMATRIX*>(&state_.world_matrix));
  IDirect3DSurface9* target = nullptr;
  device_->GetRenderTarget(0, &target);
  if (target) target->Release();
  mark_scene_started(target);
  state_.subset = ~std::uint32_t{};
  if (callbacks_.begin_scene) callbacks_.begin_scene(device_);
  return S_OK;
}
void SceneRouter::mark_scene_started(IDirect3DSurface9* target) noexcept { render_target_ = target; ended_ = false; }
void SceneRouter::finish_scene() {
  if (!ended_) {
    if (callbacks_.end_scene) callbacks_.end_scene(device_);
    ended_ = true;
  }
}
void SceneRouter::finish_if_target(IDirect3DSurface9* target) { if (target == render_target_) finish_scene(); }
HRESULT SceneRouter::draw_primitive(D3DPRIMITIVETYPE primitive, UINT start, UINT count) {
  if (!state_.disabled && !ended_) {
    if (!callbacks_.draw_primitive) return E_UNEXPECTED;
    state_.reset_draw_context();
    callbacks_.draw_primitive(device_, primitive, start, count);
    return S_OK;
  }
  return device_->DrawPrimitive(primitive, start, count);
}
HRESULT SceneRouter::draw_indexed_primitive(const HostApi& host, D3DPRIMITIVETYPE primitive, INT base, UINT minimum, UINT vertices, UINT start, UINT count) {
  if (state_.disabled || ended_) return device_->DrawIndexedPrimitive(primitive, base, minimum, vertices, start, count);
  if (!callbacks_.draw_indexed_primitive || !host.current_object || !host.current_material || !host.current_technic) return E_UNEXPECTED;
  IDirect3DVertexShader9* shader = nullptr;
  device_->GetVertexShader(&shader);
  if (shader) shader->Release();
  const int current = host.current_object();
  if (!current) state_.reset_draw_context();
  else {
    auto order = draw_orders_.find(current);
    if (order == draw_orders_.end()) return E_UNEXPECTED;
    auto model = models_.find(order->second);
    if (model == models_.end()) return E_UNEXPECTED;
    state_.current_model = model->second.id;
    state_.current_model_kind = model->second.kind;
    state_.subset = static_cast<std::uint32_t>(host.current_material());
    state_.draw_type = static_cast<std::uint32_t>(host.current_technic());
  }
  DWORD blend = D3DBLEND_INVSRCALPHA;
  device_->GetRenderState(D3DRS_DESTBLEND, &blend);
  state_.blend_mode = blend == D3DBLEND_ONE ? 1 : 0;
  state_.texture = nullptr; state_.sphere_texture = nullptr;
  if (!shader) {
    state_.effect_file_used = false;
    state_.texture_used = false; state_.sphere_used = false; state_.sphere_subtexture = false;
    state_.toon_disabled = state_.current_model_kind != ModelKind::pmd;
    state_.sphere_add = false;
    for (DWORD stage = state_.current_model_kind == ModelKind::pmd ? 1 : 0; stage < 3; ++stage) {
      DWORD operation = D3DTOP_DISABLE;
      device_->GetTextureStageState(stage, D3DTSS_COLOROP, &operation);
      if (operation == D3DTOP_DISABLE) break;
      IDirect3DBaseTexture9* texture = nullptr;
      device_->GetTexture(stage, &texture);
      if (!texture) continue;
      texture->Release();
      DWORD coordinates = 0;
      device_->GetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS, &coordinates);
      if (coordinates == D3DTTFF_COUNT2) {
        state_.sphere_texture = texture;
        state_.sphere_used = true;
        device_->GetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, &coordinates);
        if (coordinates == D3DTSS_TCI_CAMERASPACENORMAL) state_.sphere_add = operation == D3DTOP_ADD;
        else { state_.sphere_subtexture = true; state_.sphere_add = false; }
      } else { state_.texture_used = true; state_.texture = texture; }
    }
  } else {
    if (!state_.effect) return E_UNEXPECTED;
    const auto handle = state_.effect->GetCurrentTechnique();
    const auto found = state_.technique_indices.find(handle);
    // Exact flags are extracted from the retained 0x18005bdf0 table.
    const DWORD value = found != state_.technique_indices.end() && found->second < technique_table.size() ? technique_table[found->second].flags : 0;
    state_.texture_used = (value & 0xff) != 0;
    state_.sphere_used = (value & 0xff00) != 0;
    state_.sphere_subtexture = (value & 0xff0000) != 0;
    state_.toon_disabled = (value & 0xff000000) != 0;
    if (state_.texture_used) { device_->GetTexture(1, &state_.texture); if (state_.texture) state_.texture->Release(); }
    if (state_.sphere_used) { device_->GetTexture(state_.texture_used ? 2 : 1, &state_.sphere_texture); if (state_.sphere_texture) state_.sphere_texture->Release(); }
    BOOL add = FALSE;
    state_.effect->GetBool("spadd", &add);
    state_.sphere_add = add != FALSE;
    state_.effect_file_used = true;
  }
  callbacks_.draw_indexed_primitive(device_, primitive, base, minimum, vertices, start, count);
  return S_OK;
}
HRESULT SceneRouter::clear(DWORD count, const D3DRECT* rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
  IDirect3DSurface9* current = nullptr;
  device_->GetRenderTarget(0, &current);
  if (current) current->Release();
  if (render_target_ && initialized_ && !state_.disabled && callbacks_.clear) {
    const BOOL primary = current == render_target_ && (flags & D3DCLEAR_TARGET) != 0;
    if (primary) state_.clear_color = color;
    return callbacks_.clear(device_, count, rectangles, flags, color, z, stencil, primary);
  }
  return device_->Clear(count, rectangles, flags, color, z, stencil);
}
HRESULT SceneRouter::reset(D3DPRESENT_PARAMETERS* presentation) {
  const bool notify = callbacks_.lost_device && callbacks_.reset_device;
  if (notify) callbacks_.lost_device(device_);
  const auto result = device_->Reset(presentation);
  // Original calls OnResetDevice even when Reset failed.
  if (notify) callbacks_.reset_device(device_);
  if (presentation && presentation->hDeviceWindow) state_.reset_window = presentation->hDeviceWindow;
  else {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (SUCCEEDED(device_->GetCreationParameters(&creation))) state_.reset_window = creation.hFocusWindow;
  }
  return result;
}
void SceneRouter::cleanup() {
  if (callbacks_.cleanup) callbacks_.cleanup(device_);
  for (auto& pair : models_) if (pair.second.retained_resource) pair.second.retained_resource->Release();
  models_.clear();
  draw_orders_.clear();
  if (state_.active_models == &models_) state_.active_models = nullptr;
  if (state_.effect_device == device_) {
    state_.effect = nullptr;
    state_.effect_device = nullptr;
    state_.technique_indices.clear();
    state_.reset_draw_context();
    state_.texture = nullptr;
    state_.sphere_texture = nullptr;
    state_.toon_textures.clear();
  }
  render_target_ = nullptr;
  initialized_ = false;
}
}
