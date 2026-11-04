// MMSAdapter.dll - legacy MME host export bridge.
//
// The legacy effect API is 37 ExpGet*/ExpSet* exports that the running host
// publishes from its executable. MMSAdapter publishes the same names and
// ordinals from its own module and forwards every call, unchanged, to the host
// export of the same name. It is a thin wrapper only: it holds no effect state,
// loads no module, and links no host import library.
//
// Prototypes below come from the host definitions in src/exports/effect_api.cpp.
// Names and ordinals come from exports/MikuMikuDance.def (ordinals 1..37, base 1)
// and are pinned in MMSAdapter.def. All three files describe the same 37 exports.
//
// The host table is resolved once, in DLL_PROCESS_ATTACH, with
// GetModuleHandleW(nullptr) + GetProcAddress. The DLL refuses to load unless all
// 37 names resolve; after that the table is read-only and the host owns the
// module lifetime. Nothing here calls the host during load.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows 7 SP1 minimum, also when this file is compiled by another build that
// does not pass the version macros; the build below passes the same values.
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <Windows.h>

#include <cstdint>

namespace {

// One host function pointer type per export. The prototype is written once in
// exports.inc, so a thunk cannot drift from the type it forwards to.
#define MMS_ADAPTER_EXPORT(ordinal, name, ret, params, args) \
    using Fn_##name = ret(__cdecl*) params;
#include "exports.inc"
#undef MMS_ADAPTER_EXPORT

struct HostExports {
#define MMS_ADAPTER_EXPORT(ordinal, name, ret, params, args) Fn_##name name;
#include "exports.inc"
#undef MMS_ADAPTER_EXPORT
};

// Written only by ResolveHostExports, which runs before the module admits any
// call. A failed resolution returns FALSE from DllMain, so the module unloads and
// no thunk can observe a partial table.
HostExports g_host{};

template <typename Fn>
Fn HostLookup(HMODULE host, const char* symbol) noexcept {
#pragma warning(push)
// GetProcAddress returns FARPROC; this cast supplies the exact host prototype
// that exports.inc declares. It is the documented Win32 bridge.
#pragma warning(disable : 4191)
    return reinterpret_cast<Fn>(::GetProcAddress(host, symbol));
#pragma warning(pop)
}

// All-or-nothing: stage the whole table and publish it only when every name
// resolves. Runs under the loader lock, so it does no allocation, file access,
// waiting, or host business call - only the symbol lookups.
bool ResolveHostExports(HMODULE host) noexcept {
    HostExports pending{};
#define MMS_ADAPTER_EXPORT(ordinal, name, ret, params, args)         \
    pending.name = HostLookup<Fn_##name>(host, #name);               \
    if (pending.name == nullptr) return false;
#include "exports.inc"
#undef MMS_ADAPTER_EXPORT
    g_host = pending;
    return true;
}

// Drop or add a line in exports.inc and this count changes; the assertion keeps
// the wrapper set tied to the host's 37-export table.
#define MMS_ADAPTER_EXPORT(ordinal, name, ret, params, args) +1
constexpr int kHostExportCount = 0
#include "exports.inc"
    ;
#undef MMS_ADAPTER_EXPORT

static_assert(kHostExportCount == 37,
              "MMSAdapter must forward exactly the 37 host Exp* exports.");

}  // namespace

extern "C" {

// One thunk per host export. Arguments and return value pass through unchanged;
// no exception, allocation, or state crosses this C boundary. Calling one before
// DLL_PROCESS_ATTACH resolves the table is impossible: the module fails to load
// in that case.
#define MMS_ADAPTER_EXPORT(ordinal, name, ret, params, args) \
    ret __cdecl name params { return g_host.name args; }
#include "exports.inc"
#undef MMS_ADAPTER_EXPORT

}  // extern "C"

extern "C" BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD reason,
                               LPVOID /*reserved*/) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    const HMODULE host = ::GetModuleHandleW(nullptr);
    if (host == nullptr) return FALSE;
    if (!ResolveHostExports(host)) return FALSE;
    return TRUE;
}
