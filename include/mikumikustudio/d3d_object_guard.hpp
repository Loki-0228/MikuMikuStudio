#pragma once

// ===========================================================================
// Direct3D object screening for the render passes
// ===========================================================================
// The port mirrors record layouts that carry raw D3D resource pointers.  When
// such a field is never written - or is reached through an offset that only
// holds it on x86 - the renderer hands Direct3D a sentinel (typically -1) and
// the access violation surfaces deep inside d3d9.dll, far from the port call
// that passed it (the crash report then names d3d9.dll and nothing else).
//
// Every such pointer is therefore screened before it is bound: a D3D object
// lives in committed, readable user memory and starts with a vtable pointer
// that lands in a mapped image.  The vtable itself sits in the image's
// read-only data (d3dx9_43.dll's mesh vtables are a plain .rdata structure),
// so only readability - not executability - is required of it.
//
// The two VirtualQuery calls are expensive in repeated MME render passes.
// A rendering scope can reuse successful checks while retaining COM ownership;
// calls outside that scope still perform the original complete check.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Unknwn.h>
#include <array>
#include "mikumikustudio/perf_timing.hpp"

namespace mikumikustudio {

class D3dObjectValidationScope;
inline thread_local D3dObjectValidationScope* activeD3dValidation = nullptr;

// Pins validated COM objects until the rendering scope ends. An object cannot
// be freed/reused at the same address while its validation is cached. A changed
// vtable still takes the full validation path (MME may install a proxy).
class D3dObjectValidationScope {
    struct Entry { const void* object = nullptr; const void* vtable = nullptr; };
    std::array<Entry, 256> entries{};
    D3dObjectValidationScope* previous;
    static std::size_t Start(const void* object) {
        return (reinterpret_cast<ULONG_PTR>(object) >> 4) % 256;
    }
public:
    D3dObjectValidationScope() : previous(activeD3dValidation) { activeD3dValidation = this; }
    D3dObjectValidationScope(const D3dObjectValidationScope&) = delete;
    D3dObjectValidationScope& operator=(const D3dObjectValidationScope&) = delete;
    ~D3dObjectValidationScope() {
        MMD_PERF_SCOPE(ValidationRelease);
        activeD3dValidation = previous;
        for (const auto& entry : entries)
            if (entry.object)
                static_cast<IUnknown*>(const_cast<void*>(entry.object))->Release();
    }
    bool Contains(const void* object) const {
        for (std::size_t i=0, start=Start(object); i<entries.size(); ++i) {
            const auto& entry=entries[(start+i)%entries.size()];
            if (!entry.object) return false;
            if (entry.object==object)
                return entry.vtable==*static_cast<const void* const*>(object);
        }
        return false;
    }
    void Remember(const void* object, const void* vtable) {
        for (std::size_t i=0, start=Start(object); i<entries.size(); ++i) {
            auto& entry=entries[(start+i)%entries.size()];
            if (entry.object==object) { entry.vtable=vtable; return; }
            if (!entry.object) {
                static_cast<IUnknown*>(const_cast<void*>(object))->AddRef();
                entry={object,vtable};
                return;
            }
        }
        // Full table: retain the original uncached checks for other objects.
    }
};

inline bool PlausibleD3dObject(const void* object) noexcept {
    MMD_PERF_SCOPE(ResourceGuard);
    const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(object);
    if (value < 0x10000 || value > 0x00007FFFFFFFFFFFULL)
        return false;
    if (activeD3dValidation && activeD3dValidation->Contains(object))
        return true;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(object, &region, sizeof(region)) == 0 ||
        region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    const void* vtable = *reinterpret_cast<const void* const*>(object);
    if (vtable == nullptr)
        return false;
    MEMORY_BASIC_INFORMATION code{};
    if (VirtualQuery(vtable, &code, sizeof(code)) == 0 ||
        code.State != MEM_COMMIT ||
        code.Type != MEM_IMAGE ||
        (code.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    if (activeD3dValidation) activeD3dValidation->Remember(object, vtable);
    return true;
}

}  // namespace mikumikustudio
