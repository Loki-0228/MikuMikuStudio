#include "mikumikustudio/effect_file_watch.hpp"
#include "mikumikustudio/effect_apng.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "effect_file_watch_cache.hpp"
#include <MinHook.h>
#include <intrin.h>
#include <cstdlib>
#include <cstring>
#include <new>

namespace mikumikustudio::effect_file_watch {
#if defined(_WIN64)
namespace {
ReadTime original = nullptr;
const void* automaticCaller = nullptr;
DWORD thread = 0;
Cache* cache = nullptr;
bool attempted = false, disabled = false;
std::uint64_t directReads = 0;
std::uint64_t __fastcall FileTime(const char* path) {
    // REA: 0xB965 is the call in the 100 ms auto-reload branch. The other
    // callers load/compile resources or validate paths and must always read.
    if (_ReturnAddress() != automaticCaller || GetCurrentThreadId() != thread || !path)
        return original(path);
    if (disabled || !cache) { ++directReads; return original(path); }
    try { return cache->Get(path, original); }
    catch (...) { ++directReads; return original(path); }
}
}
void Install() {
    if (attempted) return;
    attempted = true;
    if (!IsEffectApngCompatibilityInstalled()) return;
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"MMEffect.dll"));
    constexpr unsigned char code[]{0x40,0x53,0x48,0x83,0xec,0x40,0x45,0x33,0xc9,0x48,0xc7,0x44,0x24,0x30,0x00,0x00};
    constexpr unsigned char call[]{0xe8,0x86,0xfe,0xff,0xff};
    if (!image || std::memcmp(image + 0xb7f0, code, sizeof(code)) ||
        std::memcmp(image + 0xb965, call, sizeof(call))) return;
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return;
    std::unique_ptr<Cache> next(new(std::nothrow) Cache);
    if (!next) return;
    if (MH_CreateHook(image + 0xb7f0, FileTime, reinterpret_cast<void**>(&original)) != MH_OK) return;
    thread = GetCurrentThreadId(); automaticCaller = image + 0xb96a;
    disabled = std::getenv("MMS_NO_MME_FILE_WATCH") != nullptr;
    cache = next.get();
    if (MH_EnableHook(image + 0xb7f0) != MH_OK) {
        MH_RemoveHook(image + 0xb7f0); cache = nullptr; return;
    }
    // The verified/pinned MME and hook live until process exit. Retain bounded
    // state for the same lifetime, including late DLL teardown callbacks.
    next.release();
    runtime_log::Write("MME_FILE_WATCH installed disabled=%d", int(disabled));
}
Stats GetStats() {
    auto result = cache ? cache->GetStats() : Stats{};
    result.reads += directReads;
    return result;
}
#else
void Install() {}
Stats GetStats() { return {}; }
#endif
}
