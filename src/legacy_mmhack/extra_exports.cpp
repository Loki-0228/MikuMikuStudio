#include "runtime.hpp"
#include "light_matrix.hpp"
#include <cstdlib>
namespace {
[[noreturn]] void invalid_host_state(DWORD code) {
  // Invalid input in the original reaches an unchecked pointer or failed
  // inverse. Report it as a non-continuable SEH error at the C ABI boundary.
  RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  std::abort();
}
}
extern "C" {
BOOL WINAPI GetAcsAttachedPmd(std::uint64_t model_id, std::uint64_t* pmd_id, int* bone) {
  if (!pmd_id || !bone) invalid_host_state(EXCEPTION_ACCESS_VIOLATION);
  auto& runtime = mme_re::mmhack::runtime();
  if (runtime.host.accessory_attachment) return runtime.host.accessory_attachment(model_id, pmd_id, bone);
  const auto backing = runtime.state.object_backing(model_id);
  struct Attachment { int model; int bone; } attachment{};
  SIZE_T read = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), backing.base + 0x240, &attachment, sizeof(attachment), &read) || read != sizeof(attachment)) invalid_host_state(EXCEPTION_ACCESS_VIOLATION);
  if (attachment.model != -1 && !runtime.host.pmd_id) invalid_host_state(EXCEPTION_ACCESS_VIOLATION);
  *pmd_id = attachment.model == -1 ? 0 : runtime.host.pmd_id(attachment.model);
  *bone = attachment.bone;
  return *pmd_id != 0 ? TRUE : FALSE;
}
void WINAPI GetLightViewProjMatrix(D3DMATRIX* view, D3DMATRIX* projection, D3DMATRIX* combined) {
  if (!view || !projection || !combined) invalid_host_state(EXCEPTION_ACCESS_VIOLATION);
  auto& runtime = mme_re::mmhack::runtime();
  const auto result = mme_re::mmhack::light_view_projection(runtime.state, runtime.host, *view, *projection, *combined);
  if (FAILED(result)) invalid_host_state(static_cast<DWORD>(result));
}
}
