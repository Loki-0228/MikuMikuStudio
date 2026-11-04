#include "d3dx_hooks.hpp"
#include "technique_table.hpp"
#include <algorithm>
#include <array>
namespace mme_re::mmhack {
namespace {
struct InstalledHook { HMODULE target; FARPROC original; FARPROC replacement; };
std::vector<InstalledHook> installed_hooks;
bool install_one(const char* name, FARPROC replacement, HMODULE target, HMODULE provider) {
  if (!target) target = GetModuleHandleW(nullptr);
  const auto original = GetProcAddress(provider, name);
  if (!original) return false;
  installed_hooks.push_back({target, original, replacement});
  return hook_import(name, replacement, target, provider).ok();
}
std::wstring ansi_to_wide(const char* value) {
  if (!value) return {};
  const int size = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
  if (!size) return L"(invalid)";
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (!MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), size)) return L"(invalid)";
  result.resize(static_cast<std::size_t>(size) - 1);
  return result;
}
bool shared_toon(const std::wstring& filename, std::wstring& basename) {
  if (filename.size() != 15 || _wcsnicmp(filename.c_str(), L"data", 4) != 0) return false;
  const auto separator = filename.find_last_of(L"/\\");
  if (separator == std::wstring::npos) return false;
  basename = filename.substr(separator + 1);
  if (basename.size() != 10 || basename.compare(0, 4, L"toon") || basename.compare(6, 4, L".bmp")) return false;
  return (basename[4] == L'0' && basename[5] >= L'1' && basename[5] <= L'9') || (basename[4] == L'1' && basename[5] == L'0');
}
}
void record_texture_output(State& state, const std::wstring& filename, IDirect3DTexture9** output) {
  std::wstring key;
  IDirect3DTexture9** captured = nullptr;
  if (filename.empty()) { key.clear(); captured = output; }
  else if (shared_toon(filename, key)) {
    captured = output;
    if (key == L"toon01.bmp" && output) state.toon_textures.emplace(L"", TextureRecord{nullptr, output - 1});
  } else {
    key = filename;
    std::replace(key.begin(), key.end(), L'\\', L'/');
  }
  state.toon_textures.emplace(std::move(key), TextureRecord{output ? *output : nullptr, captured});
}
IDirect3D9* WINAPI hooked_direct3d_create9(UINT sdk_version) { return runtime().wrap(Direct3DCreate9(sdk_version)); }
HRESULT WINAPI hooked_load_mesh_w(LPCWSTR filename, DWORD options, IDirect3DDevice9* device, ID3DXBuffer** adjacency, ID3DXBuffer** materials, ID3DXBuffer** instances, DWORD* material_count, ID3DXMesh** output) {
  // 0x1800043b0 forwards R8 unchanged. The mesh must retain the host's
  // wrapped device so DrawSubset reaches the MME draw callbacks.
  const auto result = D3DXLoadMeshFromXW(filename, options, device, adjacency, materials, instances, material_count, output);
  if (result == S_OK && output && *output) {
    runtime().state.record_host_output(output, filename ? filename : L"");
    *output = runtime().wrap(*output);
  }
  return result;
}
HRESULT WINAPI hooked_create_effect_resource_a(IDirect3DDevice9* device, HMODULE source, LPCSTR resource, const D3DXMACRO* macros, ID3DXInclude* include, DWORD flags, ID3DXEffectPool* pool, ID3DXEffect** output, ID3DXBuffer** errors) {
  const auto result = D3DXCreateEffectFromResourceA(runtime().unwrap(device), source, resource, macros, include, flags, pool, output, errors);
  auto& state = runtime().state;
  if (result == S_OK && !state.effect && output && *output) {
    state.technique_indices.clear();
    for (std::size_t i = 0; i < technique_table.size(); ++i) if (const auto handle = (*output)->GetTechniqueByName(technique_table[i].name)) state.technique_indices[handle] = i;
    state.effect_device = runtime().unwrap(device);
    state.effect = *output;
  }
  return result;
}
HRESULT WINAPI hooked_create_effect(IDirect3DDevice9* device, LPCVOID data, UINT size, const D3DXMACRO* macros, ID3DXInclude* include, DWORD flags, ID3DXEffectPool* pool, ID3DXEffect** output, ID3DXBuffer** errors) {
  const auto result = D3DXCreateEffect(runtime().unwrap(device), data, size, macros, include, flags, pool, output, errors);
  auto& state = runtime().state;
  if (SUCCEEDED(result) && !state.effect && output && *output) {
    // Only the host standard effect has this full technique table.
    bool standard = true;
    for (const auto& technique : technique_table) standard = standard && (*output)->GetTechniqueByName(technique.name) != nullptr;
    if (standard) {
      state.technique_indices.clear();
      for (std::size_t i = 0; i < technique_table.size(); ++i) state.technique_indices[(*output)->GetTechniqueByName(technique_table[i].name)] = i;
      state.effect_device = runtime().unwrap(device);
      state.effect = *output;
    }
  }
  return result;
}
HRESULT WINAPI hooked_texture_file_a(IDirect3DDevice9* device, LPCSTR file, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mip_filter, D3DCOLOR color_key, D3DXIMAGE_INFO* source, PALETTEENTRY* palette, IDirect3DTexture9** output) {
  const auto result = D3DXCreateTextureFromFileExA(runtime().unwrap(device), file, width, height, levels, usage, format, pool, filter, mip_filter, color_key, source, palette, output);
  record_texture_output(runtime().state, ansi_to_wide(file), output);
  return result;
}
HRESULT WINAPI hooked_texture_file_w(IDirect3DDevice9* device, LPCWSTR file, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mip_filter, D3DCOLOR color_key, D3DXIMAGE_INFO* source, PALETTEENTRY* palette, IDirect3DTexture9** output) {
  const auto result = D3DXCreateTextureFromFileExW(runtime().unwrap(device), file, width, height, levels, usage, format, pool, filter, mip_filter, color_key, source, palette, output);
  record_texture_output(runtime().state, file ? file : L"", output);
  return result;
}
HRESULT WINAPI hooked_texture_memory(IDirect3DDevice9* device, LPCVOID source, UINT source_size, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, DWORD filter, DWORD mip_filter, D3DCOLOR key, D3DXIMAGE_INFO* info, PALETTEENTRY* palette, IDirect3DTexture9** output) {
  return D3DXCreateTextureFromFileInMemoryEx(runtime().unwrap(device), source, source_size, width, height, levels, usage, format, pool, filter, mip_filter, key, info, palette, output);
}
HRESULT WINAPI hooked_create_texture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** output) { return D3DXCreateTexture(runtime().unwrap(device), width, height, levels, usage, format, pool, output); }
HRESULT WINAPI hooked_load_mesh_memory(LPCVOID source, DWORD size, DWORD options, IDirect3DDevice9* device, ID3DXBuffer** adjacency, ID3DXBuffer** materials, ID3DXBuffer** instances, DWORD* count, ID3DXMesh** output) {
  // 0x48a0 simply forwards; it does not register or wrap the returned mesh.
  return D3DXLoadMeshFromXInMemory(source, size, options, device, adjacency, materials, instances, count, output);
}
bool install_host_hooks(std::string& failed_import) {
  struct Hook { const char* provider; const char* name; FARPROC replacement; };
  const Hook hooks[] = {
    {"d3d9.dll", "Direct3DCreate9", reinterpret_cast<FARPROC>(&hooked_direct3d_create9)},
    {"kernel32.dll", "CreateFileW", reinterpret_cast<FARPROC>(&tracked_create_file_w)},
    {"d3dx9_43.dll", "D3DXLoadMeshFromXW", reinterpret_cast<FARPROC>(&hooked_load_mesh_w)},
    {"d3dx9_43.dll", "D3DXCreateEffect", reinterpret_cast<FARPROC>(&hooked_create_effect)},
    {"d3dx9_43.dll", "D3DXCreateEffectFromResourceA", reinterpret_cast<FARPROC>(&hooked_create_effect_resource_a)},
    {"d3dx9_43.dll", "D3DXCreateTextureFromFileExA", reinterpret_cast<FARPROC>(&hooked_texture_file_a)},
    {"d3dx9_43.dll", "D3DXCreateTextureFromFileExW", reinterpret_cast<FARPROC>(&hooked_texture_file_w)},
    {"d3dx9_43.dll", "D3DXCreateTextureFromFileInMemoryEx", reinterpret_cast<FARPROC>(&hooked_texture_memory)},
    {"d3dx9_43.dll", "D3DXCreateTexture", reinterpret_cast<FARPROC>(&hooked_create_texture)},
    {"d3dx9_43.dll", "D3DXLoadMeshFromXInMemory", reinterpret_cast<FARPROC>(&hooked_load_mesh_memory)},
    {"user32.dll", "GetKeyState", reinterpret_cast<FARPROC>(&filtered_get_key_state)}
  };
  for (std::size_t i = 0; i < std::size(hooks); ++i) {
    const auto& hook = hooks[i];
    HMODULE provider = GetModuleHandleA(hook.provider);
    if (!provider || !install_one(hook.name, hook.replacement, nullptr, provider)) { failed_import = hook.name; uninstall_host_hooks(); return false; }
    if (i == 1) {
      HMODULE crt = GetModuleHandleA("MSVCR90.dll");
      if (crt && !install_one(hook.name, hook.replacement, crt, provider)) { failed_import = "MSVCR90.dll!CreateFileW"; uninstall_host_hooks(); return false; }
    }
  }
  failed_import.clear();
  return true;
}
void uninstall_host_hooks() noexcept {
  for (auto iterator = installed_hooks.rbegin(); iterator != installed_hooks.rend(); ++iterator) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(iterator->target);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    replace_import_address(iterator->target, nt->OptionalHeader.SizeOfImage, iterator->replacement, iterator->original);
  }
  installed_hooks.clear();
}
}
