#include "../render/nvidia_antialias_elevation.hpp"
namespace detail = mikumikustudio::nvidia_antialias_fix::detail;

namespace {
// Derive identity from the DLL, including when Windows loads it in regsvr32.
bool SiblingApplication(std::wstring& application) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&SiblingApplication), &module)) return false;
    std::wstring helper;
    if (!detail::CanonicalFile(detail::ModulePath(module), helper)) return false;
    const auto requested = helper.substr(0, helper.find_last_of(L'\\') + 1) + detail::kApplicationName;
    return detail::CanonicalFile(requested, application) &&
        detail::NormalizePath(requested) == detail::NormalizePath(application) &&
        detail::IsSiblingApplication(helper, application);
}
HRESULT Change(bool enabled) noexcept {
    try {
        std::wstring application;
        if (!SiblingApplication(application)) return E_INVALIDARG;
        if (!detail::IsElevated()) return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);
        mikumikustudio::nvidia_antialias_fix::State state;
        return detail::RunForPath(application, &enabled, state) && state.enabled == enabled ? S_OK : E_FAIL;
    } catch (...) { return E_FAIL; }
}
}
// Loading the DLL only exposes its API; registration requires an explicit call.
extern "C" HRESULT WINAPI DllRegisterServer() { return Change(true); }
extern "C" HRESULT WINAPI DllUnregisterServer() { return Change(false); }
extern "C" HRESULT WINAPI MMSNvidiaAAFixQueryInstalled(BOOL* installed) {
    if (!installed) return E_POINTER;
    *installed = FALSE;
    try {
        std::wstring application;
        if (!SiblingApplication(application)) return E_INVALIDARG;
        detail::Driver driver;
        if (!driver.ready) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        mikumikustudio::nvidia_antialias_fix::State state;
        if (!detail::Execute(driver.api, application, nullptr, state)) return E_FAIL;
        *installed = state.enabled ? TRUE : FALSE;
        return S_OK;
    } catch (...) { return E_FAIL; }
}
