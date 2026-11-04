#include "state.hpp"
#include <cwchar>
#include <utility>

namespace mme_re::mmhack {
IDirect3DTexture9* TextureRecord::resolve() noexcept {
  if (!texture && creation_output) texture = *creation_output;
  return texture;
}
State& state() noexcept { static State instance; return instance; }
void State::record_host_output(void* output_slot, const std::wstring& filename) {
  const auto address = reinterpret_cast<std::uintptr_t>(output_slot);
  for (std::uintptr_t offset = 0; offset < 64; offset += 8) {
    const auto candidate = address - offset;
    const ModelId key = static_cast<std::uint32_t>(candidate);
    object_backings[key] = {reinterpret_cast<std::byte*>(candidate), filename};
  }
}
const ObjectBacking State::object_backing(ModelId id) const {
  auto found = object_backings.find(id);
  if (found != object_backings.end()) return found->second;
  return {reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(id)), {}};
}
IUnknown* State::discover_resource(ModelId id, ModelKind kind) const {
  const auto backing = object_backing(id);
  for (std::size_t i = 0; i < 8; ++i) {
    IUnknown* candidate = nullptr;
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), backing.base + i * sizeof(void*), &candidate, sizeof(candidate), &read) || read != sizeof(candidate) || !candidate) continue;
    const auto& known = kind == ModelKind::pmd ? index_buffers : wrapped_meshes;
    if (known.find(candidate) != known.end()) return candidate;
  }
  return nullptr;
}
void State::reset_draw_context() noexcept {
  subset = ~std::uint32_t{};
  current_model = 0;
  current_model_kind = ModelKind::accessory;
  draw_type = 0;
  effect_file_used = false;
}
IDirect3DTexture9* State::toon_texture() {
  if (current_model_kind != ModelKind::pmd) return nullptr;
  auto find_texture = [this](const std::wstring& key) -> IDirect3DTexture9* {
    auto found = toon_textures.find(key);
    return found == toon_textures.end() ? nullptr : found->second.resolve();
  };
  const auto& current_models = active_models ? *active_models : models;
  auto model = current_models.find(current_model);
  if (model != current_models.end() && subset < model->second.materials.size()) {
    const auto& name = model->second.materials[subset].toon_texture;
    if (!name.empty()) {
      // The fallback to toon10 only occurs when the requested key exists.
      auto found = toon_textures.find(name);
      if (found != toon_textures.end()) {
        if (auto result = found->second.resolve()) return result;
        if (auto result = find_texture(L"toon10.bmp")) return result;
      }
    }
  }
  return find_texture(L"");
}
const wchar_t* State::material_name(ModelId model, std::uint32_t index, bool alternate) const {
  const auto& current_models = active_models ? *active_models : models;
  auto found = current_models.find(model);
  if (found == current_models.end() || found->second.kind != ModelKind::pmd || index >= found->second.materials.size()) return nullptr;
  const auto& value = alternate ? found->second.materials[index].alternate_name : found->second.materials[index].name;
  return value.empty() ? nullptr : value.c_str();
}
void State::observe_file_open(const wchar_t* filename, DWORD access) {
  if (!filename) return;
  if (access == GENERIC_READ) last_read_file = filename;
  const auto length = std::wcslen(filename);
  // The original subtracts four without a length check. Keep valid-input
  // behavior while avoiding an out-of-bounds read on a short path.
  if (length < 4 || _wcsicmp(filename + length - 4, L".pmm") != 0) return;
  if (access == GENERIC_READ) pending_loaded_pmm = filename;
  else if (access == GENERIC_WRITE) pending_saved_pmms.emplace_back(filename);
}
const wchar_t* State::take_loaded_pmm() {
  loaded_result.clear();
  if (pending_loaded_pmm.empty()) return nullptr;
  loaded_result = std::move(pending_loaded_pmm);
  pending_loaded_pmm.clear();
  return loaded_result.c_str();
}
const wchar_t* State::take_saved_pmm() {
  saved_result.clear();
  if (pending_saved_pmms.empty()) return nullptr;
  saved_result = std::move(pending_saved_pmms.front());
  pending_saved_pmms.pop_front();
  return saved_result.c_str();
}
SHORT filter_key_state(int key, SHORT (WINAPI* original)(int)) {
  const SHORT value = original(key);
  if (key == 'E' && value < 0 && original(VK_CONTROL) < 0 && original(VK_SHIFT) < 0) return 0;
  return value;
}
SHORT WINAPI filtered_get_key_state(int key) { return filter_key_state(key, &::GetKeyState); }
HANDLE WINAPI tracked_create_file_w(LPCWSTR file, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
  state().observe_file_open(file, access);
  return ::CreateFileW(file, access, share, security, creation, flags, template_file);
}
}
using mme_re::mmhack::state;
extern "C" {
std::uint64_t WINAPI GetCurrentModelID() { return static_cast<std::uint32_t>(state().current_model); }
std::uint32_t WINAPI GetCurrentDrawType() { return state().draw_type; }
HWND WINAPI GetMMDMainWindow() { return state().main_window; }
HWND WINAPI GetDrawnWindow() { return state().drawn_window; }
BOOL WINAPI IsEditMode() { return state().playing ? FALSE : TRUE; }
float WINAPI GetCurrentFrameTime() { return state().frame_time; }
IDirect3DBaseTexture9* WINAPI GetTexture() { return state().texture; }
IDirect3DTexture9* WINAPI GetToonTexture() { return state().toon_texture(); }
IDirect3DBaseTexture9* WINAPI GetSphereMapTexture() { return state().sphere_texture; }
std::uint8_t WINAPI GetSphereMapMode() { return !state().sphere_used ? 0 : state().sphere_subtexture ? 3 : state().sphere_add ? 2 : 1; }
BOOL WINAPI IsToonUsed() { return (state().draw_type == 1 || state().draw_type == 2) && !state().toon_disabled ? TRUE : FALSE; }
const wchar_t* WINAPI GetMaterialName(std::uint64_t model, std::uint32_t index, int alternate) { return state().material_name(model, index, alternate != 0); }
ID3DXEffect* WINAPI GetCurrentEffect() { return state().effect; }
BOOL WINAPI IsEffectFileUsed() { return state().effect_file_used ? TRUE : FALSE; }
std::uint32_t WINAPI GetCurrentSubsetIndex() { return state().subset; }
std::uint32_t WINAPI GetBlendMode() { return state().blend_mode; }
BOOL WINAPI IsDebugMode() { return state().debug ? TRUE : FALSE; }
D3DCOLOR WINAPI GetClearColor() { return state().clear_color; }
const wchar_t* WINAPI LoadedPMMFile() { return state().take_loaded_pmm(); }
const wchar_t* WINAPI SavedPMMFile() { return state().take_saved_pmm(); }
}
