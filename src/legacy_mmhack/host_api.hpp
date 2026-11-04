#pragma once
#include <windows.h>
#include <d3d9.h>
#include <string>
#include <cstdint>

namespace mme_re::mmhack {
// Signatures are inferred from the call sites in 0x27f0, 0x2de0 and 0x1330.
// They are recovery interfaces, not a claim that the original SDK was found.
struct HostApi {
  int (WINAPI* current_object)() = nullptr;
  int (WINAPI* accessory_count)() = nullptr;
  int (WINAPI* accessory_order)(int) = nullptr;
  float* (__cdecl* accessory_world)(float*, int) = nullptr;
  int (WINAPI* current_material)() = nullptr;
  int (WINAPI* current_technic)() = nullptr;
  float (WINAPI* frame_time)() = nullptr;
  int (WINAPI* pmd_count)() = nullptr;
  unsigned int (WINAPI* pmd_id)(int) = nullptr;
  unsigned int (WINAPI* accessory_id)(int) = nullptr;
  const char* (WINAPI* pmd_filename)(int) = nullptr;
  int (WINAPI* pmd_material_count)(int) = nullptr;
  const char* (WINAPI* accessory_filename)(int) = nullptr;
  int (WINAPI* accessory_material_count)(int) = nullptr;
  int (WINAPI* pmd_order)(int) = nullptr;
  void* (__cdecl* object_backing)(int kind, int index, const wchar_t** path, IUnknown** resource) = nullptr;
  BOOL (__cdecl* accessory_attachment)(std::uint64_t id, std::uint64_t*, int*) = nullptr;
  // Transactional: on an unresolved name no partially bound API is published.
  bool bind(HMODULE executable, std::string& missing);
};
}
