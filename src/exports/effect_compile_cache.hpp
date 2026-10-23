// ===========================================================================
// effect_compile_cache.hpp - compiled D3DX effect bytecode cache for MME
// ===========================================================================
// MME builds its external effects with D3DXCreateEffectFromFileW on every
// launch.  The field Ray/ToonGlass project creates 45 effect files per launch
// and the call takes about 5.5 s in total (compilation plus object creation).
// The cache keys source, dependencies, compiler, flags and macros. It keeps
// immutable compiled bytecode in memory and in a private disk cache: every
// call still creates its own ID3DXEffect through the same D3DX DLL with the
// caller's device, pool and flags.  No ID3DXEffect is shared, and no COM, GPU
// or compiler work moves to another thread.
//
// The cache is consulted only for the default include handler (pInclude ==
// nullptr).  With a custom ID3DXInclude the original entry point runs, so the
// caller's handler observes exactly the include requests it would otherwise
// see.  The original call also remains the fallback for every bypass or
// failure; see TryCreateEffect below.
//
// Include lookup follows the DLL that owns the original entry: D3DX9_32 uses
// cwd; D3DX9_43 tries the main source directory, then cwd. Nested includes use
// the same rule. A key-only include handler records raw bytes and resolved
// paths, while compilation uses the DLL's original file compiler. It does not
// change cwd. Unknown D3DX versions bypass the cache.
//
// The first warning-producing compile also calls the original entry to retain
// its exact diagnostics (D3DX43 can suppress parser warnings). Hits create a
// fresh diagnostic buffer as well as a fresh effect. This adds work on a cold
// warning-producing input; subsequent hits avoid compilation.
//
// Key material (every variable-length field is length-prefixed):
//   * cache format version,
//   * SHA-256 of the loaded D3DX DLL file that owns the original
//     D3DXCreateEffectFromFileW address (the actual caller compiler version,
//     never a hard-coded d3dx9_43),
//   * absolute source path, caller spelling, cwd and raw source digest,
//   * D3DXSHADER flags,
//   * macro names and values,
//   * complete text produced by D3DXPreprocessShaderFromFileW, so any #include
//     content change changes the key even when the include timestamp does not,
//   * resolved dependency paths and raw digests, including comments/line breaks.
//
// Disk format (little endian):
//   uint32 magic          bytes 'M','M','E','C'
//   uint32 format version kCacheFormatVersion
//   uint32 keyBytes       1 .. kMaxKeyBytes
//   uint32 payloadBytes   1 .. kMaxPayloadBytes
//   uint8  keySha256[32]
//   uint8  payloadSha256[32]
//   uint8  key[keyBytes]
//   uint8  payload[payloadBytes]
// Reads are bounded, both digests and the stored key are verified, and a
// damaged, truncated or foreign file counts as a miss: unchecked bytes are
// never handed to D3DX.  Writes use one unique temporary file in the cache
// directory and an atomic rename. The cache layout is
// <EXE directory>\Cache\MME\<effect filename without extension>\<key SHA256>.fxb.
// Unicode effect names are preserved; invalid/reserved/overlong folder names
// are sanitized. The full key still separates paths, macros and compilers.
// Format version 3 remains inside the file header and key. Only this named
// layout is read; no migration from earlier unpublished layouts is provided.
// The disk budget covers all effects.
// MMS_EFFECT_CACHE_DIR overrides the MME root
// for the focused tests only, and MMS_EFFECT_CACHE_DISABLE=1 turns the cache
// off as a diagnostic fallback.  An unwritable directory falls back to the
// in-memory cache and then to the original entry point; no project file is
// ever written.
//
// Diagnostics: one short runtime_log line per outcome
// (MME_EFFECT_CACHE hit/miss/store/bypass/fallback) with the reason and the
// first 16 hex digits of the key digest.  A cache problem never escapes the
// WINAPI boundary as an exception and is never reported as a load failure.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio::effect_compile_cache {

// Disk format version.  Bump when the key layout or the file layout changes;
// old entries then simply miss instead of being misread.
inline constexpr std::uint32_t kCacheFormatVersion = 3;

// Sanity bounds for input and cache sizes.  Exceeding one is a bypass or a
// skipped store, never an error returned to the caller.
inline constexpr std::size_t kMaxPreprocessedBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaxPayloadBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxMacroDefinitionBytes = 16u * 1024u;
inline constexpr std::size_t kMaxMacroCount = 256;

// ID3DXBuffer: IUnknown (slots 0..2) then GetBufferPointer (3) and
// GetBufferSize (4); see d3dx9core.h.  ID3DXEffectCompiler::CompileEffect is
// slot 59 (IUnknown + 53 ID3DXBaseEffect entries + SetLiteral/GetLiteral).
using FnGetBufferPointer = void* (WINAPI*)(void* buffer);
using FnGetBufferSize = DWORD (WINAPI*)(void* buffer);
using FnRelease = ULONG (WINAPI*)(void* object);
using FnCompileEffect = HRESULT (WINAPI*)(void* compiler, DWORD flags,
                                          void** effect, void** errors);

// Exact d3dx9shader.h / d3dx9effect.h WINAPI signatures, declared without the
// DirectX SDK headers (the project does not depend on them).
using FnPreprocessFromFileW = HRESULT (WINAPI*)(
    LPCWSTR file, const void* macros, const void* include, void** text,
    void** errors);
using FnCreateEffectCompiler = HRESULT (WINAPI*)(
    const char* source, UINT length, const void* macros, const void* include,
    DWORD flags, void** compiler, void** errors);
using FnCreateEffectCompilerFromFileW = HRESULT (WINAPI*)(
    LPCWSTR file, const void* macros, const void* include,
    DWORD flags, void** compiler, void** errors);
using FnCreateEffect = HRESULT (WINAPI*)(
    IDirect3DDevice9* device, LPCVOID data, UINT length, const void* macros,
    const void* include, DWORD flags, void* pool, void** effect, void** errors);
using FnCreateEffectFromFileW = HRESULT (WINAPI*)(
    IDirect3DDevice9* device, LPCWSTR file, const void* macros,
    const void* include, DWORD flags, void* pool, void** effect, void** errors);
using FnCreateBuffer = HRESULT (WINAPI*)(DWORD bytes, void** buffer);

// Export table of one loaded D3DX DLL plus its file identity.  The identity is
// the SHA-256 of the DLL file itself, computed once per module.
struct D3DX {
    void* module = nullptr;
    FnPreprocessFromFileW preprocessFromFile = nullptr;
    FnCreateEffectCompiler createCompiler = nullptr;
    FnCreateEffectCompilerFromFileW createCompilerFromFile = nullptr;
    FnCreateEffectFromFileW originalFromFile = nullptr;
    FnCreateBuffer createBuffer = nullptr;
    FnCreateEffect createEffect = nullptr;
    FnCompileEffect compileEffect = nullptr;
    FnGetBufferPointer bufferPointer = nullptr;
    FnGetBufferSize bufferSize = nullptr;
    FnRelease release = nullptr;
    std::array<unsigned char, 32> identity{};
    bool identityValid = false;
    bool includesRelativeToSource = false;
};

// Resolves the exports from the module that owns createEffectFromFile (the
// original D3DXCreateEffectFromFileW address captured from MME's import).  No
// library is loaded: the module must already be loaded and its file name must
// be d3dx9_*.dll.  Returns false when the address cannot be attributed to a
// loaded D3DX DLL, an export is missing, or the DLL identity cannot be read.
bool ResolveD3DX(void* createEffectFromFile, D3DX& hooks) noexcept;

// Production entry point: resolves the D3DX exports of createEffectFromFile and
// tries the cached path.  Returns true when the call was fully handled (a cache
// hit, or a miss that compiled and created the effect).  Returns false when the
// caller must invoke the original D3DXCreateEffectFromFileW: cache disabled,
// custom include handler, parameters that D3DX owns (null device/filename/
// output), unreadable macros/path, preprocessing failure or diagnostics,
// unsupported compiler version, oversized input,
// compile/creation failure, or any cache exception.
//
// On true, *effect and any returned diagnostic buffer belong to the caller.
// On false, the outputs are untouched and GetLastError is restored to
// the value seen on entry, so the original call keeps its own semantics.
// Once the effect object has been created, cache publication is best effort:
// a storage or memory-map failure is only logged and the call still returns
// true with that object, so a cache problem can never fail a load.
bool TryCreateEffect(void* createEffectFromFile, IDirect3DDevice9* device,
                     LPCWSTR filename, const void* macros, const void* include,
                     DWORD flags, void* pool, void** effect, void** errors,
                     HRESULT& result) noexcept;

// Test seam: the same cached path with an explicit D3DX export table.  The
// focused regression test substitutes fake compiler/buffer COM objects here to
// count CompileEffect and effect-creation calls without a D3DX runtime.
bool TryCreateEffectWithHooks(const D3DX& hooks, IDirect3DDevice9* device,
                              LPCWSTR filename, const void* macros,
                              const void* include, DWORD flags, void* pool,
                              void** effect, void** errors,
                              HRESULT& result) noexcept;

// Cache directory actually used: MMS_EFFECT_CACHE_DIR when set, otherwise
// <EXE directory>\Cache\MME. Effects use named subdirectories below this root.
// Empty if the executable path is unavailable.
std::wstring CacheDirectory() noexcept;

// Drops the in-process bytecode map.  The focused tests use it to force the
// next lookup through the disk cache; production code does not call it.
void ClearMemoryCache() noexcept;

// Test-only disk capacity override.  Zero keeps the production limit for that
// dimension, so SetDiskLimitsForTesting(0, 0) restores the defaults.  Production
// code never calls it.
void SetDiskLimitsForTesting(std::size_t entries, std::uint64_t bytes) noexcept;

// Test-only fault injection.  The named stage throws std::runtime_error once;
// the exception is handled exactly like an allocation failure.  Production code
// never sets a fault point.
enum class FaultPoint {
    kNone = 0,
    kBeforePreprocessedCopy,  // preprocessed buffer held, text copy not started
    kBeforeBytecodeCopy,      // compiled buffer held, payload copy not started
    kBeforeCachePublish,      // effect created, cache publication not started
};
void SetFaultPointForTesting(FaultPoint point) noexcept;

}  // namespace mikumikustudio::effect_compile_cache
