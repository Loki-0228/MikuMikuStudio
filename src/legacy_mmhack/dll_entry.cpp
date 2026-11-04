#include "d3dx_hooks.hpp"
#include <filesystem>
#include <vector>
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  try {
    std::string error;
    auto& runtime = mme_re::mmhack::runtime();
    if (!runtime.host.bind(GetModuleHandleW(nullptr), error)) return FALSE;
    if (!mme_re::mmhack::install_host_hooks(error)) return FALSE;
    // Keep this off the loader thread stack: real MMD loads MMHack during a
    // deep DllMain chain, where a 64 KB local array can exhaust the stack.
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) { mme_re::mmhack::uninstall_host_hooks(); return FALSE; }
    const auto directory = std::filesystem::path(path.data()).parent_path();
    if (!mme_re::mmhack::load_effect_callbacks(directory.wstring(), runtime.callbacks, runtime.state.debug, error)) {
      mme_re::mmhack::uninstall_host_hooks();
      MessageBoxA(nullptr, "Initialize error", "MikuMikuEffect", MB_OK);
      return FALSE;
    }
    return TRUE;
  } catch (...) { mme_re::mmhack::uninstall_host_hooks(); return FALSE; }
}
