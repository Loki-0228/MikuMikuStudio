#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ID3DXEffect;

namespace mme_re::mmhack {
using ModelId = std::uint64_t;
enum class ModelKind : std::uint32_t { accessory = 0, pmd = 1 };
struct MaterialNames {
  std::wstring toon_texture;
  std::wstring name;
  std::wstring alternate_name;
};
struct ModelRecord {
  ModelId id = 0;
  ModelKind kind = ModelKind::accessory;
  std::vector<MaterialNames> materials;
  std::string filename;
  IUnknown* retained_resource = nullptr;
};
struct ObjectBacking { std::byte* base = nullptr; std::wstring filename; };
using ModelMap = std::unordered_map<ModelId, ModelRecord>;
// Both fields are needed: 0xf810 lazily dereferences the captured texture output.
// These are borrowed pointers, matching the original getters' absence of AddRef.
struct TextureRecord {
  IDirect3DTexture9* texture = nullptr;
  IDirect3DTexture9** creation_output = nullptr;
  IDirect3DTexture9* resolve() noexcept;
};
struct State {
  ModelId current_model = 0;                 // DAT_18006e868
  ModelKind current_model_kind = ModelKind::accessory; // DAT_18006e870
  std::uint32_t draw_type = 0;                // DAT_18006e798
  HWND main_window = nullptr;                // DAT_18006e7e0
  HWND drawn_window = nullptr;               // DAT_18006e7c0
  HWND reset_window = nullptr;               // DAT_18006e7c8
  bool playing = false;                      // DAT_18006e796
  float frame_time = 0;                      // DAT_18006e79c
  IDirect3DBaseTexture9* texture = nullptr;   // DAT_18006e7a8
  IDirect3DBaseTexture9* sphere_texture = nullptr; // DAT_18006e7b0
  bool sphere_used = false;                  // DAT_18006e789
  bool sphere_subtexture = false;            // DAT_18006e78a
  bool sphere_add = false;                   // DAT_18006e797
  bool toon_disabled = false;                // DAT_18006e78b
  bool effect_file_used = false;             // DAT_18006e795
  IDirect3DDevice9* effect_device = nullptr; // borrowed standard-effect owner
  ID3DXEffect* effect = nullptr;             // DAT_18006e7a0
  std::uint32_t subset = ~std::uint32_t{};    // DAT_18006ccc0
  std::uint32_t blend_mode = 0;              // DAT_18006e7b8
  bool debug = false;                        // DAT_18006e7bc
  bool disabled = false;                     // DAT_18006e794
  D3DCOLOR clear_color = 0;                  // DAT_18006ccc4
  D3DMATRIX world_matrix{};                   // DAT_18006e8b0
  D3DMATRIX world_inverse{};                  // DAT_18006e8f0
  HWND frame_edit = nullptr;                  // DAT_18006e840
  bool recording_modal = false;               // DAT_18006e7bd
  bool texture_used = false;                  // DAT_18006e788
  std::unordered_map<const char*, std::size_t> technique_indices;
  ModelMap models;
  const ModelMap* active_models = nullptr;     // DAT_18006e7d0 selects a device's table
  std::unordered_map<ModelId, ObjectBacking> object_backings;
  std::unordered_set<IUnknown*> index_buffers;
  std::unordered_set<IUnknown*> wrapped_meshes;
  std::unordered_map<std::wstring, TextureRecord> toon_textures;
  std::wstring last_read_file;
  std::wstring pending_loaded_pmm;
  std::deque<std::wstring> pending_saved_pmms;
  std::wstring loaded_result;
  std::wstring saved_result;

  void reset_draw_context() noexcept;
  IDirect3DTexture9* toon_texture();
  const wchar_t* material_name(ModelId model, std::uint32_t index, bool alternate) const;
  void observe_file_open(const wchar_t* filename, DWORD desired_access);
  const wchar_t* take_loaded_pmm();
  const wchar_t* take_saved_pmm();
  void record_host_output(void* output_slot, const std::wstring& filename);
  const ObjectBacking object_backing(ModelId id) const;
  IUnknown* discover_resource(ModelId id, ModelKind kind) const;
};
State& state() noexcept;
// A separate state argument permits deterministic tests and multiple analyses.
SHORT filter_key_state(int key, SHORT (WINAPI* original)(int));
HANDLE WINAPI tracked_create_file_w(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
SHORT WINAPI filtered_get_key_state(int key);
}

// These exports are shared by the static component tests and the rebuilt DLL.
// Boolean exports use BOOL32 deliberately: consumers of EAX and AL both receive
// canonical zero/one. This is a restored compatibility contract, not evidence
// that the original source used the BOOL typedef.
extern "C" {
std::uint64_t WINAPI GetCurrentModelID();
std::uint32_t WINAPI GetCurrentDrawType();
HWND WINAPI GetMMDMainWindow();
HWND WINAPI GetDrawnWindow();
BOOL WINAPI IsEditMode();
float WINAPI GetCurrentFrameTime();
IDirect3DBaseTexture9* WINAPI GetTexture();
IDirect3DTexture9* WINAPI GetToonTexture();
IDirect3DBaseTexture9* WINAPI GetSphereMapTexture();
std::uint8_t WINAPI GetSphereMapMode();
BOOL WINAPI IsToonUsed();
const wchar_t* WINAPI GetMaterialName(std::uint64_t, std::uint32_t, int);
ID3DXEffect* WINAPI GetCurrentEffect();
BOOL WINAPI IsEffectFileUsed();
std::uint32_t WINAPI GetCurrentSubsetIndex();
std::uint32_t WINAPI GetBlendMode();
BOOL WINAPI IsDebugMode();
D3DCOLOR WINAPI GetClearColor();
const wchar_t* WINAPI LoadedPMMFile();
const wchar_t* WINAPI SavedPMMFile();
BOOL WINAPI GetAcsAttachedPmd(std::uint64_t, std::uint64_t*, int*);
void WINAPI GetLightViewProjMatrix(D3DMATRIX*, D3DMATRIX*, D3DMATRIX*);
}
