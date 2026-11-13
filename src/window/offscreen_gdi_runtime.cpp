#include "mikumikustudio/offscreen_gdi_runtime.hpp"
#include <Windows.h>
#include <new>

namespace mikumikustudio {
namespace {
std::uint64_t CurrentThread(void*) noexcept { return GetCurrentThreadId(); }
std::uintptr_t AcquireCurrentThread(void*) noexcept {
    HANDLE thread = nullptr;
    return DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                           0, FALSE, DUPLICATE_SAME_ACCESS)
        ? reinterpret_cast<std::uintptr_t>(thread) : 0;
}
bool ThreadIsAlive(void*, std::uintptr_t thread) noexcept {
    return WaitForSingleObject(reinterpret_cast<HANDLE>(thread), 0) == WAIT_TIMEOUT;
}
}
OffscreenGdiRuntimeCore<16>* ProcessOffscreenGdiRuntime() noexcept {
    // Both the thread identity and runtime survive until process exit.
    static auto* binding = new (std::nothrow) OffscreenRuntimeThreadBinding(
        {nullptr, CurrentThread, AcquireCurrentThread, ThreadIsAlive});
    static auto* runtime = binding ? new (std::nothrow) OffscreenGdiRuntimeCore<16>(binding->Thread()) : nullptr;
    return runtime;
}
OffscreenSurfaceOps ProcessOffscreenGdiOps() noexcept { return Win32OffscreenSurfaceOps(); }
}
