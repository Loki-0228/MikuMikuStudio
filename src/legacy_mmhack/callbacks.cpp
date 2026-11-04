#include "callbacks.hpp"
namespace mme_re::mmhack {
bool Callbacks::bind(HMODULE module, std::string& missing) {
  if (!module) { missing = "MMEffect.dll"; return false; }
  Callbacks candidate;
#define MME_CALLBACK(field, name) candidate.field = reinterpret_cast<decltype(candidate.field)>(GetProcAddress(module, name)); if (!candidate.field) { missing = name; return false; }
  MME_CALLBACK(begin_scene, "OnBeginScene")
  MME_CALLBACK(end_scene, "OnEndScene")
  MME_CALLBACK(draw_primitive, "OnDrawPrimitive")
  MME_CALLBACK(draw_indexed_primitive, "OnDrawIndexedPrimitive")
  MME_CALLBACK(initialize, "Initialize")
  MME_CALLBACK(cleanup, "Cleanup")
  MME_CALLBACK(create_model, "OnCreateModel")
  MME_CALLBACK(delete_model, "OnDeleteModel")
  MME_CALLBACK(lost_device, "OnLostDevice")
  MME_CALLBACK(reset_device, "OnResetDevice")
  MME_CALLBACK(clear, "OnClear")
#undef MME_CALLBACK
  *this = candidate;
  missing.clear();
  return true;
}
HMODULE load_effect_callbacks(const std::wstring& directory, Callbacks& callbacks, bool& debug, std::string& error) {
  if (directory.empty()) { error = "MMEffect directory is empty"; return nullptr; }
  const auto separator = directory.back() == L'\\' || directory.back() == L'/' ? L"" : L"\\";
  const auto prefix = directory + separator;
  HMODULE module = LoadLibraryW((prefix + L"MMEffect.dll").c_str());
  if (!module) { error = "LoadLibraryW(MMEffect.dll) failed"; return nullptr; }
  if (!callbacks.bind(module, error)) { FreeLibrary(module); return nullptr; }
  debug = GetFileAttributesW((prefix + L"MMEffect.debug").c_str()) != INVALID_FILE_ATTRIBUTES;
  return module;
}
}
