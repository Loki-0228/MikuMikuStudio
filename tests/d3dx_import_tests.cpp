// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/d3dx_dyn.hpp"
#include <cstdio>
#include <cstdlib>

using namespace mikumikustudio;

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS) && defined(_WIN64)
// Test only: replace one import in this test executable, as an effect proxy
// does after device creation. Restore the slot before exiting the test.
extern "C" d3dx::FnMatrixOp2 __imp_D3DXMatrixMultiply;
namespace {
bool hookCalled = false;
d3dx::D3DXMATRIXF* WINAPI Hook(d3dx::D3DXMATRIXF* out,
                              const d3dx::D3DXMATRIXF*, const d3dx::D3DXMATRIXF*) {
    hookCalled = true;
    return out;
}
__declspec(noinline) bool Reload(d3dx::Api& api) { return api.Load(); }
}
#endif

int main() {
#if defined(MIKUMIKUSTUDIO_D3DX_STATIC_IMPORTS) && defined(_WIN64)
    d3dx::Api api;
    Check(Reload(api), "load original imports");
    const HMODULE originalModule = api.module;
    auto* slot = &__imp_D3DXMatrixMultiply;
    const auto original = *slot;
    DWORD protection = 0;
    Check(VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection) != 0,
          "make test import writable");
    *slot = Hook;
    const bool loaded = Reload(api);
    d3dx::D3DXMATRIXF matrix{};
    if (loaded) api.multiply(&matrix, &matrix, &matrix);
    *slot = original;
    DWORD discarded = 0;
    Check(VirtualProtect(slot, sizeof(*slot), protection, &discarded) != 0,
          "restore import page protection");
    Check(loaded && hookCalled, "late IAT hook is observed after module caching");
    Check(api.module == originalModule, "module handle remains stable");
    Check(Reload(api) && api.multiply == original, "removed IAT hook is observed");
    std::puts("PASS late D3DX import hook installation and removal");
#else
    std::puts("SKIP: static x64 D3DX imports are disabled");
#endif
}
