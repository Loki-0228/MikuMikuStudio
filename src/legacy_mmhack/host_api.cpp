#include "host_api.hpp"
namespace mme_re::mmhack {
bool HostApi::bind(HMODULE executable, std::string& missing) {
  if (!executable) executable = GetModuleHandleW(nullptr);
  HostApi candidate;
#define MME_BIND(field, name) candidate.field = reinterpret_cast<decltype(candidate.field)>(GetProcAddress(executable, name)); if (!candidate.field) { missing = name; return false; }
  MME_BIND(current_object, "ExpGetCurrentObject")
  MME_BIND(accessory_count, "ExpGetAcsNum")
  MME_BIND(accessory_order, "ExpGetAcsOrder")
  MME_BIND(accessory_world, "ExpGetAcsWorldMat")
  MME_BIND(current_material, "ExpGetCurrentMaterial")
  MME_BIND(current_technic, "ExpGetCurrentTechnic")
  MME_BIND(frame_time, "ExpGetFrameTime")
  MME_BIND(pmd_count, "ExpGetPmdNum")
  MME_BIND(pmd_id, "ExpGetPmdID")
  MME_BIND(accessory_id, "ExpGetAcsID")
  MME_BIND(pmd_filename, "ExpGetPmdFilename")
  MME_BIND(pmd_material_count, "ExpGetPmdMatNum")
  MME_BIND(accessory_filename, "ExpGetAcsFilename")
  MME_BIND(accessory_material_count, "ExpGetAcsMatNum")
  MME_BIND(pmd_order, "ExpGetPmdOrder")
#undef MME_BIND
  candidate.object_backing = reinterpret_cast<decltype(candidate.object_backing)>(GetProcAddress(executable, "MMSLegacyObjectBacking"));
  candidate.accessory_attachment = reinterpret_cast<decltype(candidate.accessory_attachment)>(GetProcAddress(executable, "MMSLegacyAccessoryAttachment"));
  *this = candidate;
  missing.clear();
  return true;
}
}
