// ===========================================================================
// effect_compile_cache.cpp - compiled D3DX effect bytecode cache for MME
// ===========================================================================
// See effect_compile_cache.hpp for the contract, key material and disk layout.
// This file owns three pieces:
//   * the D3DX export/identity resolution of the loaded compiler DLL,
//   * the key/disk/memory cache primitives,
//   * the call wrapper that replaces one D3DXCreateEffectFromFileW invocation
//     with preprocess -> lookup -> compile -> create.
// Every path that cannot safely finish returns false to the caller, which then
// invokes the original entry point.  No exception crosses the WINAPI boundary,
// and a cache problem after a successful effect creation never degrades the
// load: the created effect is committed and returned as success.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "effect_compile_cache.hpp"

#include <bcrypt.h>
#include <Windows.h>

#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/cache_storage_policy.hpp"
#include "mikumikustudio/cache_manager.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mikumikustudio::effect_compile_cache {
namespace {

// Disk entry layout and the limits that keep a corrupt file from becoming a
// large allocation or an invalid D3DX call.
constexpr std::uint32_t kCacheFileMagic = 0x43454D4Du;  // bytes M M E C
constexpr std::size_t kHeaderBytes = 4u * 4u + 32u + 32u;
constexpr std::size_t kMaxMacroNameBytes = 512;
constexpr std::size_t kMaxPathBytes = 32u * 1024u;
constexpr std::size_t kMaxKeyBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxCacheFileBytes =
    kHeaderBytes + kMaxKeyBytes + kMaxPayloadBytes;
constexpr std::size_t kMaxDiskEntries = 512;
constexpr std::uint64_t kMaxDiskBytes = 256ull * 1024ull * 1024ull;
constexpr std::size_t kMaxMemoryEntries = 64;
constexpr std::size_t kMaxMemoryBytes = 32u * 1024u * 1024u;

std::atomic<std::size_t> g_diskEntryLimit{kMaxDiskEntries};
std::atomic<std::uint64_t> g_diskByteLimit{kMaxDiskBytes};

// Test-only fault injection: one named stage throws std::runtime_error.
std::atomic<int> g_faultPoint{static_cast<int>(FaultPoint::kNone)};

void ThrowIfFault(FaultPoint point) {
    if (g_faultPoint.load(std::memory_order_relaxed) !=
        static_cast<int>(point))
        return;
    g_faultPoint.store(static_cast<int>(FaultPoint::kNone),
                       std::memory_order_relaxed);
    throw std::runtime_error("effect_compile_cache fault injection");
}

// ---------------------------------------------------------------------------
// SHA-256 (bcrypt; the core already links bcrypt)
// ---------------------------------------------------------------------------
bool Sha256(const void* data, std::size_t size,
            std::array<unsigned char, 32>& digest) noexcept {
    if (!data || size == 0) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) >= 0) {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
            ok = BCryptHashData(hash,
                                static_cast<PUCHAR>(const_cast<void*>(data)),
                                static_cast<ULONG>(size), 0) >= 0;
            ok = ok && BCryptFinishHash(hash, digest.data(),
                                        static_cast<ULONG>(digest.size()),
                                        0) >= 0;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool Sha256File(const std::wstring& path,
                std::array<unsigned char, 32>& digest,
                std::uint64_t maximumBytes = UINT64_MAX) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct FileGuard {
        HANDLE value;
        ~FileGuard() {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    } guard{file};
    LARGE_INTEGER fileBytes{};
    if (!GetFileSizeEx(file, &fileBytes) || fileBytes.QuadPart < 0 ||
        static_cast<std::uint64_t>(fileBytes.QuadPart) > maximumBytes)
        return false;
    // This helper also runs during texture preparation on the GUI thread.
    // Avoid adding a 32 KiB frame to the legacy model-loading call stack.
    constexpr DWORD chunkBytes = 32u * 1024u;
    std::unique_ptr<unsigned char[]> chunk(new (std::nothrow) unsigned char[chunkBytes]);
    if (!chunk) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::uint64_t totalRead = 0;
        ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, chunk.get(), chunkBytes, &read, nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) break;
            totalRead += read;
            if (totalRead > maximumBytes) {
                ok = false;
                break;
            }
            if (BCryptHashData(hash, chunk.get(), read, 0) < 0) {
                ok = false;
                break;
            }
        }
        if (ok)
            ok = BCryptFinishHash(hash, digest.data(),
                                  static_cast<ULONG>(digest.size()), 0) >= 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::string KeyText(const std::array<unsigned char, 32>& digest) {
    return std::string(reinterpret_cast<const char*>(digest.data()),
                       digest.size());
}

std::string ShortKey(const std::array<unsigned char, 32>& digest) {
    static const char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(16);
    for (std::size_t index = 0; index < 8; ++index) {
        text.push_back(hex[digest[index] >> 4]);
        text.push_back(hex[digest[index] & 0x0Fu]);
    }
    return text;
}

void LogBypass(const char* reason) noexcept {
    runtime_log::Write("MME_EFFECT_CACHE bypass reason=%s", reason);
}

void LogFallback(const char* reason, const char* shortKey) noexcept {
    runtime_log::Write("MME_EFFECT_CACHE fallback reason=%s key=%s", reason,
                       shortKey);
}

// Releases every temporary COM object exactly once, including when a later
// std::vector copy throws.  Detach is used only when the object is handed to
// the caller.
class ScopedObject {
public:
    explicit ScopedObject(const D3DX& hooks,
                          void* object = nullptr) noexcept
        : hooks_(hooks), object_(object) {}
    ~ScopedObject() { Reset(); }
    ScopedObject(const ScopedObject&) = delete;
    ScopedObject& operator=(const ScopedObject&) = delete;
    void** Address() noexcept { return &object_; }
    void* Get() const noexcept { return object_; }
    void Reset() noexcept {
        if (object_ && hooks_.release) hooks_.release(object_);
        object_ = nullptr;
    }
    void* Detach() noexcept {
        void* object = object_;
        object_ = nullptr;
        return object;
    }

private:
    const D3DX& hooks_;
    void* object_ = nullptr;
};

// ---------------------------------------------------------------------------
// D3DX DLL identity and export resolution
// ---------------------------------------------------------------------------
struct ModuleIdentityEntry {
    HMODULE module = nullptr;
    std::array<unsigned char, 32> digest{};
    bool valid = false;
};

std::mutex identityMutex;
std::vector<ModuleIdentityEntry> moduleIdentities;

// A loaded DLL file cannot be replaced while it is mapped, so one hash per
// module is enough for the process lifetime.
bool ModuleIdentity(HMODULE module, std::array<unsigned char, 32>& digest) {
    std::lock_guard<std::mutex> lock(identityMutex);
    for (const auto& entry : moduleIdentities) {
        if (entry.module == module) {
            digest = entry.digest;
            return entry.valid;
        }
    }
    ModuleIdentityEntry entry;
    entry.module = module;
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length && length < path.size())
        entry.valid = Sha256File(std::wstring(path.data(), length), entry.digest);
    digest = entry.digest;
    if (moduleIdentities.size() < 4) moduleIdentities.push_back(entry);
    return entry.valid;
}

bool StartsWithD3dx(const wchar_t* name) noexcept {
    static const wchar_t prefix[] = L"d3dx9_";
    for (std::size_t index = 0; index < 6; ++index) {
        wchar_t current = name[index];
        if (current >= L'A' && current <= L'Z')
            current = static_cast<wchar_t>(current - L'A' + L'a');
        if (current != prefix[index]) return false;
    }
    return true;
}

// ID3DXBuffer / ID3DXEffectCompiler vtable slot dispatch.  Resolving these
// through the object's own vtable keeps one honest release function for every
// D3DX object and avoids depending on a returned interface's concrete type.
template <typename Function>
Function VtableSlot(void* object, std::size_t index) noexcept {
    void** table = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Function>(table[index]);
}

ULONG WINAPI ReleaseObjectSlot(void* object) noexcept {
    using Function = ULONG(WINAPI*)(void*);
    return VtableSlot<Function>(object, 2)(object);
}

void* WINAPI BufferPointerSlot(void* buffer) noexcept {
    using Function = void*(WINAPI*)(void*);
    return VtableSlot<Function>(buffer, 3)(buffer);
}

DWORD WINAPI BufferSizeSlot(void* buffer) noexcept {
    using Function = DWORD(WINAPI*)(void*);
    return VtableSlot<Function>(buffer, 4)(buffer);
}

HRESULT WINAPI CompileEffectSlot(void* compiler, DWORD flags, void** effect,
                                 void** errors) noexcept {
    using Function = HRESULT(WINAPI*)(void*, DWORD, void**, void**);
    return VtableSlot<Function>(compiler, 59)(compiler, flags, effect, errors);
}

bool ResolveD3DXImpl(void* createEffectFromFile, D3DX& hooks) {
    hooks = D3DX{};
    if (!createEffectFromFile) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(createEffectFromFile),
                            &module) ||
        !module)
        return false;
    std::vector<wchar_t> modulePath(32768);
    const DWORD length =
        GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) return false;
    const wchar_t* name = std::wcsrchr(modulePath.data(), L'\\');
    name = name ? name + 1 : modulePath.data();
    // A hooked entry point lives in a proxy module and must never be treated
    // as the compiler; only a real loaded d3dx9_*.dll is acceptable.
    if (!StartsWithD3dx(name) ||
        (_wcsicmp(name, L"d3dx9_32.dll") != 0 &&
         _wcsicmp(name, L"d3dx9_43.dll") != 0))
        return false;
    hooks.module = module;
    hooks.includesRelativeToSource = _wcsicmp(name, L"d3dx9_43.dll") == 0;
    hooks.preprocessFromFile = reinterpret_cast<FnPreprocessFromFileW>(
        GetProcAddress(module, "D3DXPreprocessShaderFromFileW"));
    hooks.createCompiler = reinterpret_cast<FnCreateEffectCompiler>(
        GetProcAddress(module, "D3DXCreateEffectCompiler"));
    hooks.createCompilerFromFile = reinterpret_cast<FnCreateEffectCompilerFromFileW>(
        GetProcAddress(module, "D3DXCreateEffectCompilerFromFileW"));
    hooks.originalFromFile = reinterpret_cast<FnCreateEffectFromFileW>(createEffectFromFile);
    hooks.createBuffer = reinterpret_cast<FnCreateBuffer>(GetProcAddress(module, "D3DXCreateBuffer"));
    hooks.createEffect = reinterpret_cast<FnCreateEffect>(
        GetProcAddress(module, "D3DXCreateEffect"));
    hooks.compileEffect = &CompileEffectSlot;
    hooks.bufferPointer = &BufferPointerSlot;
    hooks.bufferSize = &BufferSizeSlot;
    hooks.release = &ReleaseObjectSlot;
    if (!hooks.preprocessFromFile || !hooks.createCompiler || !hooks.createCompilerFromFile ||
        !hooks.originalFromFile || !hooks.createBuffer ||
        !hooks.createEffect || !hooks.compileEffect || !hooks.bufferPointer ||
        !hooks.bufferSize || !hooks.release)
        return false;
    hooks.identityValid = ModuleIdentity(module, hooks.identity);
    return hooks.identityValid;
}

// ---------------------------------------------------------------------------
// In-process bytecode map (used on every call, and when the disk is unwritable)
// ---------------------------------------------------------------------------
struct MemoryEntry {
    std::vector<unsigned char> payload;
    std::size_t bytes = 0;
};

std::mutex memoryMutex;
std::unordered_map<std::string, MemoryEntry> memoryEntries;
std::list<std::string> memoryOrder;
std::size_t memoryBytes = 0;

bool MemoryGet(const std::array<unsigned char, 32>& digest,
               std::vector<unsigned char>& payload) noexcept {
    try {
        std::lock_guard<std::mutex> lock(memoryMutex);
        const auto found = memoryEntries.find(KeyText(digest));
        if (found == memoryEntries.end()) return false;
        payload = found->second.payload;
        return true;
    } catch (...) {
        return false;
    }
}

void MemoryPut(const std::array<unsigned char, 32>& digest,
               const std::vector<unsigned char>& payload) noexcept {
    try {
        if (payload.empty() || payload.size() > kMaxPayloadBytes ||
            payload.size() > kMaxMemoryBytes)
            return;
        std::lock_guard<std::mutex> lock(memoryMutex);
        const std::string key = KeyText(digest);
        if (memoryEntries.find(key) != memoryEntries.end()) return;
        while (!memoryOrder.empty() &&
               (memoryEntries.size() >= kMaxMemoryEntries ||
                memoryBytes + payload.size() > kMaxMemoryBytes)) {
            const auto oldest = memoryOrder.front();
            memoryOrder.pop_front();
            const auto found = memoryEntries.find(oldest);
            if (found != memoryEntries.end()) {
                memoryBytes -= found->second.bytes;
                memoryEntries.erase(found);
            }
        }
        MemoryEntry entry;
        entry.payload = payload;
        entry.bytes = payload.size();
        memoryBytes += entry.bytes;
        memoryEntries.emplace(key, std::move(entry));
        memoryOrder.push_back(key);
    } catch (...) {
        // A failed insert only loses a future hit.
    }
}

// ---------------------------------------------------------------------------
// Environment knobs and cache directory
// ---------------------------------------------------------------------------
bool CacheDisabled() noexcept {
    if (!cache_manager::Enabled()) return true;
    wchar_t value[8]{};
    const DWORD length =
        GetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DISABLE", value,
                                _countof(value));
    return length == 1 && value[0] == L'1';
}

std::wstring EntryPath(const std::wstring& directory,
                       const std::array<unsigned char, 32>& digest) {
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring name;
    name.reserve(68);
    for (const unsigned char byte : digest) {
        name.push_back(hex[byte >> 4]);
        name.push_back(hex[byte & 0x0Fu]);
    }
    name += L".fxb";
    return directory + L"\\" + name;
}

std::wstring EffectDirectory(const std::wstring& root, LPCWSTR filename) {
    if (root.empty()) return {};
    std::wstring name = std::filesystem::path(filename).stem().wstring();
    for (wchar_t& value : name) {
        if (value < 32 || std::wcschr(L"<>:\"/\\|?*", value)) value = L'_';
    }
    // Keep readable Unicode names without exceeding the component limit or
    // cutting a UTF-16 surrogate pair. The full source identity stays in the key.
    if (name.size() > 80) {
        name.resize(80);
        if (name.back() >= 0xD800 && name.back() <= 0xDBFF) name.pop_back();
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
        name.pop_back();
    if (name.empty()) name = L"effect";
    const auto deviceName = name.substr(0, name.find(L'.'));
    const bool numberedDevice = deviceName.size() == 4 &&
        (_wcsnicmp(deviceName.c_str(), L"COM", 3) == 0 ||
         _wcsnicmp(deviceName.c_str(), L"LPT", 3) == 0) &&
        deviceName[3] >= L'1' && deviceName[3] <= L'9';
    if (numberedDevice || _wcsicmp(deviceName.c_str(), L"CON") == 0 ||
        _wcsicmp(deviceName.c_str(), L"PRN") == 0 ||
        _wcsicmp(deviceName.c_str(), L"AUX") == 0 ||
        _wcsicmp(deviceName.c_str(), L"NUL") == 0)
        name.insert(name.begin(), L'_');
    return (std::filesystem::path(root) / name).wstring();
}

bool SamePath(const std::wstring& left, const std::wstring& right) noexcept {
    return left.size() == right.size() &&
        CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
            right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

// The budget belongs to the entire MME cache, not to each individual effect.
bool CountDiskEntries(const std::wstring& directory, const std::wstring& target,
                      bool descend, std::size_t maxEntries, std::uint64_t maxBytes,
                      std::size_t& visited, std::size_t& entries,
                      std::uint64_t& bytes) {
    const auto pattern = (std::filesystem::path(directory) / L"*").wstring();
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    struct SearchGuard {
        HANDLE value;
        ~SearchGuard() { FindClose(value); }
    } guard{search};
    do {
        if (!std::wcscmp(found.cFileName, L".") ||
            !std::wcscmp(found.cFileName, L"..")) continue;
        if (++visited > 4096) return false;
        const auto candidate = std::filesystem::path(directory) / found.cFileName;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (descend && ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !CountDiskEntries(candidate.wstring(), target, false, maxEntries,
                    maxBytes, visited, entries, bytes))) return false;
            continue;
        }
        if (_wcsicmp(candidate.extension().c_str(), L".fxb") != 0 ||
            SamePath(candidate.wstring(), target)) continue;
        ++entries;
        bytes += (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) |
                 found.nFileSizeLow;
        if (entries >= maxEntries || bytes > maxBytes) return false;
    } while (FindNextFileW(search, &found));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

// incomingBytes includes the header. A same-key replacement excludes the old
// file from the current total before accounting for the new file.
bool DiskBudgetAllows(const std::wstring& directory,
                      const std::wstring& target,
                      std::uint64_t incomingBytes) {
    const std::size_t maxEntries =
        g_diskEntryLimit.load(std::memory_order_relaxed);
    const std::uint64_t maxBytes =
        g_diskByteLimit.load(std::memory_order_relaxed);
    if (maxEntries == 0 || incomingBytes > maxBytes) return false;
    std::uint64_t existingBytes = 0;
    std::size_t existingEntries = 0;
    std::size_t visited = 0;
    return CountDiskEntries(directory, target, true, maxEntries, maxBytes,
                            visited, existingEntries, existingBytes) &&
        existingEntries + 1 <= maxEntries && existingBytes + incomingBytes <= maxBytes;
}

std::uint32_t ReadU32(const unsigned char* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void AppendU32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
    bytes.push_back(static_cast<unsigned char>(value >> 16));
    bytes.push_back(static_cast<unsigned char>(value >> 24));
}

bool WriteAll(HANDLE file, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t written = 0;
    while (written < size) {
        const std::size_t remaining = size - written;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x10000000u ? 0x10000000u : remaining);
        DWORD count = 0;
        if (!WriteFile(file, bytes + written, chunk, &count, nullptr) ||
            count != chunk)
            return false;
        written += count;
    }
    return true;
}

// Reads one entry.  Every header field is validated before the payload is
// copied, and the stored key plus both digests must match.  Any mismatch is a
// miss and the file is left for the next store to replace.
bool ReadCacheEntry(const std::wstring& path,
                    const std::vector<unsigned char>& key,
                    std::vector<unsigned char>& payload) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct FileGuard {
        HANDLE value;
        ~FileGuard() {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    } guard{file};
    struct Validation {
        const std::wstring& path;
        bool valid=false;
        ~Validation(){if(!valid)cache_manager::Invalidate(path);}
    } validation{path};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) ||
        size.QuadPart < static_cast<LONGLONG>(kHeaderBytes) ||
        size.QuadPart > static_cast<LONGLONG>(kMaxCacheFileBytes))
        return false;
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                  nullptr) ||
        read != bytes.size())
        return false;
    const unsigned char* data = bytes.data();
    if (ReadU32(data) != kCacheFileMagic ||
        ReadU32(data + 4) != kCacheFormatVersion)
        return false;
    const std::size_t keyBytes = ReadU32(data + 8);
    const std::size_t payloadBytes = ReadU32(data + 12);
    if (keyBytes == 0 || keyBytes > kMaxKeyBytes || payloadBytes == 0 ||
        payloadBytes > kMaxPayloadBytes)
        return false;
    if (kHeaderBytes + keyBytes + payloadBytes != bytes.size()) return false;
    if (keyBytes != key.size()) return false;
    const unsigned char* storedKey = data + kHeaderBytes;
    const unsigned char* storedPayload = storedKey + keyBytes;
    std::array<unsigned char, 32> digest{};
    if (!Sha256(storedKey, keyBytes, digest) ||
        std::memcmp(digest.data(), data + 16, digest.size()) != 0)
        return false;
    if (!Sha256(storedPayload, payloadBytes, digest) ||
        std::memcmp(digest.data(), data + 48, digest.size()) != 0)
        return false;
    if (std::memcmp(storedKey, key.data(), keyBytes) != 0) return false;
    payload.assign(storedPayload, storedPayload + payloadBytes);
    validation.valid=true;
    return true;
}

enum class StoreOutcome {
    kStored,
    kPayloadTooLarge,
    kBudgetFull,
    kUnwritable,
};

volatile LONG temporarySerial = 0;
std::atomic<bool> budgetReported{false};
std::atomic<bool> unwritableReported{false};

StoreOutcome StorePayload(const std::wstring& root, const std::wstring& directory,
                          const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& payload,
                          const std::array<unsigned char, 32>& keyDigest) {
    if (payload.empty() || payload.size() > kMaxPayloadBytes || key.empty() ||
        key.size() > kMaxKeyBytes)
        return StoreOutcome::kPayloadTooLarge;
    if (directory.empty()) return StoreOutcome::kUnwritable;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(directory),
                                        error);
    if (error) return StoreOutcome::kUnwritable;
    const std::wstring entryPath = EntryPath(directory, keyDigest);
    // The full size of this write, including the header, counts against the
    // byte budget; a same-name rewrite replaces the old file instead of
    // adding to it.
    const std::uint64_t incomingBytes =
        static_cast<std::uint64_t>(kHeaderBytes) + key.size() + payload.size();
    cache_manager::WriteReservation reservation(entryPath,incomingBytes);
    if (!reservation || (!cache_manager::Managed(entryPath) && !DiskBudgetAllows(root, entryPath, incomingBytes)))
        return StoreOutcome::kBudgetFull;
    std::array<unsigned char, 32> storedKeyDigest{};
    std::array<unsigned char, 32> payloadDigest{};
    if (!Sha256(key.data(), key.size(), storedKeyDigest) ||
        !Sha256(payload.data(), payload.size(), payloadDigest))
        return StoreOutcome::kUnwritable;
    std::vector<unsigned char> bytes;
    bytes.reserve(kHeaderBytes + key.size() + payload.size());
    AppendU32(bytes, kCacheFileMagic);
    AppendU32(bytes, kCacheFormatVersion);
    AppendU32(bytes, static_cast<std::uint32_t>(key.size()));
    AppendU32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), storedKeyDigest.begin(), storedKeyDigest.end());
    bytes.insert(bytes.end(), payloadDigest.begin(), payloadDigest.end());
    bytes.insert(bytes.end(), key.begin(), key.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    // One unique temporary name per write; concurrent writers never share a
    // file and the final rename is atomic on the same volume.
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring temporary;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG serial = InterlockedIncrement(&temporarySerial);
        temporary = entryPath + L".tmp-" +
                    std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(static_cast<long long>(serial));
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS &&
            createError != ERROR_ALREADY_EXISTS)
            return StoreOutcome::kUnwritable;
    }
    if (file == INVALID_HANDLE_VALUE) return StoreOutcome::kUnwritable;
    bool ok = WriteAll(file, bytes.data(), bytes.size());
    if (ok && !FlushFileBuffers(file)) ok = false;
    CloseHandle(file);
    if (ok)
        ok = MoveFileExW(temporary.c_str(), entryPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING |
                             MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    if(ok)reservation.Commit();
    return ok ? StoreOutcome::kStored : StoreOutcome::kUnwritable;
}

void StoreOnDiskAndLog(const std::wstring& root, const std::wstring& directory,
                       const std::vector<unsigned char>& key,
                       const std::vector<unsigned char>& payload,
                       const std::array<unsigned char, 32>& keyDigest,
                       const std::string& shortKey) {
    if (directory.empty()) return;
    switch (StorePayload(root, directory, key, payload, keyDigest)) {
        case StoreOutcome::kStored:
            runtime_log::Write("MME_EFFECT_CACHE store key=%s payload=%u",
                               shortKey.c_str(),
                               static_cast<unsigned>(payload.size()));
            break;
        case StoreOutcome::kPayloadTooLarge:
            runtime_log::Write(
                "MME_EFFECT_CACHE store skipped reason=payload-too-large");
            break;
        case StoreOutcome::kBudgetFull:
            if (!budgetReported.exchange(true, std::memory_order_relaxed))
                runtime_log::Write(
                    "MME_EFFECT_CACHE store skipped reason=cache-full");
            break;
        case StoreOutcome::kUnwritable:
            if (!unwritableReported.exchange(true, std::memory_order_relaxed))
                runtime_log::Write(
                    "MME_EFFECT_CACHE store skipped reason=cache-unwritable");
            break;
    }
}

// ---------------------------------------------------------------------------
// Key material
// ---------------------------------------------------------------------------
struct MacroEntry {
    std::string name;
    std::string value;
    bool hasValue = false;
};

struct D3DXMacroEntry {
    const char* name;
    const char* definition;
};

// Length-prefixed key writer.  The prefix is what keeps e.g. {"A","BC"} and
// {"AB","C"} distinct instead of concatenating them to the same text.
class KeyBuilder {
public:
    void U8(std::uint8_t value) { Append(&value, sizeof(value)); }
    void U32(std::uint32_t value) {
        unsigned char bytes[4] = {
            static_cast<unsigned char>(value),
            static_cast<unsigned char>(value >> 8),
            static_cast<unsigned char>(value >> 16),
            static_cast<unsigned char>(value >> 24)};
        Append(bytes, sizeof(bytes));
    }
    void Raw(const void* data, std::size_t size) { Append(data, size); }
    void Segment(const void* data, std::size_t size) {
        if (size > 0xFFFFFFFFu) {
            overflow_ = true;
            return;
        }
        U32(static_cast<std::uint32_t>(size));
        Append(data, size);
    }
    bool Valid() const noexcept { return !overflow_ && !bytes_.empty(); }
    std::vector<unsigned char> Take() { return std::move(bytes_); }

private:
    void Append(const void* data, std::size_t size) {
        if (overflow_) return;
        if (size == 0) return;
        if (size > kMaxKeyBytes || bytes_.size() > kMaxKeyBytes - size) {
            overflow_ = true;
            return;
        }
        const auto* first = static_cast<const unsigned char*>(data);
        bytes_.insert(bytes_.end(), first, first + size);
    }
    std::vector<unsigned char> bytes_;
    bool overflow_ = false;
};

bool BuildKey(const D3DX& hooks, const std::string& path, DWORD flags,
              const std::vector<MacroEntry>& macros,
              const std::vector<unsigned char>& text,
              const std::vector<unsigned char>& sourceIdentity,
              std::vector<unsigned char>& key) {
    KeyBuilder builder;
    builder.U32(kCacheFormatVersion);
    builder.Raw(hooks.identity.data(), hooks.identity.size());
    builder.Segment(path.data(), path.size());
    builder.U32(flags);
    builder.U32(static_cast<std::uint32_t>(macros.size()));
    for (const auto& macro : macros) {
        builder.Segment(macro.name.data(), macro.name.size());
        builder.U8(macro.hasValue ? 1 : 0);
        if (macro.hasValue) builder.Segment(macro.value.data(), macro.value.size());
    }
    builder.Segment(text.data(), text.size());
    builder.Segment(sourceIdentity.data(), sourceIdentity.size());
    if (!builder.Valid()) return false;
    key = builder.Take();
    return !key.empty();
}

// ---------------------------------------------------------------------------
// Bounded reads of caller-owned D3DX inputs (never trust a pointer)
// ---------------------------------------------------------------------------
std::size_t ReadableBytes(const void* address) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(address, &information, sizeof(information)) == 0) return 0;
    if (information.State != MEM_COMMIT) return 0;
    if (information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY;
    if (!(information.Protect & readable)) return 0;
    const auto base = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
    const auto current = reinterpret_cast<std::uintptr_t>(address);
    if (current < base || current >= base + information.RegionSize) return 0;
    return static_cast<std::size_t>(base + information.RegionSize - current);
}

bool ReadBoundedString(const void* address, std::size_t limit,
                       std::string& text) {
    text.clear();
    if (!address || limit == 0) return false;
    std::size_t offset = 0;
    unsigned char chunk[128];
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    while (offset < limit) {
        const void* current = reinterpret_cast<const void*>(start + offset);
        const std::size_t readable = ReadableBytes(current);
        if (readable == 0) return false;
        std::size_t want = limit - offset;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        if (want > readable) want = readable;
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), current, chunk, want,
                               &read) ||
            read != want)
            return false;
        for (std::size_t index = 0; index < want; ++index) {
            if (chunk[index] == '\0') {
                text.append(reinterpret_cast<const char*>(chunk), index);
                return true;
            }
        }
        text.append(reinterpret_cast<const char*>(chunk), want);
        offset += want;
    }
    return false;  // no terminator within the sanity bound
}

bool ReadMacros(const void* macros, std::vector<MacroEntry>& entries) {
    entries.clear();
    if (!macros) return true;
    const auto start = reinterpret_cast<std::uintptr_t>(macros);
    std::size_t totalBytes = 0;
    for (std::size_t index = 0; index < kMaxMacroCount; ++index) {
        D3DXMacroEntry raw{};
        SIZE_T read = 0;
        const void* entryAddress =
            reinterpret_cast<const void*>(start + index * sizeof(raw));
        if (!ReadProcessMemory(GetCurrentProcess(), entryAddress, &raw,
                               sizeof(raw), &read) ||
            read != sizeof(raw))
            return false;
        if (!raw.name) return true;  // NULL name terminates D3DXMACRO arrays
        MacroEntry entry;
        if (!ReadBoundedString(raw.name, kMaxMacroNameBytes, entry.name))
            return false;
        if (raw.definition) {
            entry.hasValue = true;
            if (!ReadBoundedString(raw.definition, kMaxMacroDefinitionBytes,
                                   entry.value))
                return false;
        }
        totalBytes += entry.name.size() + entry.value.size() + 16;
        if (totalBytes > kMaxKeyBytes / 2) return false;
        entries.push_back(std::move(entry));
    }
    return false;  // no terminator within the sanity bound
}

bool FullPathUtf8(LPCWSTR filename, std::string& utf8) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFullPathNameW(
        filename, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) return false;
    if (length > kMaxPathBytes) return false;
    const std::wstring path(buffer.data(), length);
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(),
        static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    std::string text(static_cast<std::size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(),
                             static_cast<int>(path.size()), text.data(), bytes,
                             nullptr, nullptr))
        return false;
    if (text.size() > kMaxPathBytes) return false;
    utf8 = std::move(text);
    return true;
}

// ---------------------------------------------------------------------------
// Cached-path implementation
// ---------------------------------------------------------------------------
bool BufferHasText(const D3DX& hooks, void* buffer) noexcept {
    if (!buffer || !hooks.bufferSize) return false;
    return hooks.bufferSize(buffer) > 0;
}

// ID3DXInclude has exactly two virtual methods, and is not an IUnknown.
// D3DX9_32's effect-file entry opens default includes relative to the process
// working directory. D3DX9_43 tries the main source directory first, then cwd;
// nested includes use the same rule, not the including file's directory.
// This key-only handler records the selected files. Compilation still uses
// the original DLL's file entry with its native default include handler.
// The integration test compares this choice against the unwrapped entry.
class EffectFileIncludes {
public:
    EffectFileIncludes(LPCWSTR filename, bool sourceRelative) {
        if (sourceRelative)
            sourceDirectory_ = std::filesystem::absolute(filename).parent_path();
    }
    virtual HRESULT WINAPI Open(int, LPCSTR name, LPCVOID,
                                LPCVOID* data, UINT* bytes) noexcept {
        if (!name || !data || !bytes) return E_INVALIDARG;
        *data = nullptr;
        *bytes = 0;
        HANDLE file = INVALID_HANDLE_VALUE;
        try {
            if (opens_++ >= 256) return E_FAIL;
            const int wideCount = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
            if (wideCount <= 0) return E_FAIL;
            std::wstring wideName(wideCount, L'\0');
            if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wideName.data(), wideCount))
                return E_FAIL;
            wideName.pop_back();
            std::filesystem::path openedPath(wideName);
            if (!sourceDirectory_.empty() && !openedPath.has_root_path()) {
                const auto candidate = sourceDirectory_ / openedPath;
                file = CreateFileW(candidate.c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (file != INVALID_HANDLE_VALUE) openedPath = candidate;
            }
            if (file == INVALID_HANDLE_VALUE)
                file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE) return E_FAIL;
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
                size.QuadPart > static_cast<LONGLONG>(kMaxPreprocessedBytes) ||
                total_ + size.QuadPart > kMaxPayloadBytes) {
                CloseHandle(file);
                return E_FAIL;
            }
            auto storage = std::make_unique<std::vector<unsigned char>>(
                static_cast<std::size_t>(size.QuadPart) + 1, 0);
            DWORD read = 0;
            const bool ok = ReadFile(file, storage->data(),
                static_cast<DWORD>(size.QuadPart), &read, nullptr) != FALSE;
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            if (!ok || read != size.QuadPart) return E_FAIL;
            std::array<unsigned char, 32> digest{};
            if (read && !Sha256(storage->data(), read, digest)) return E_FAIL;
            std::vector<wchar_t> absolutePath(32768);
            const DWORD pathLength = GetFullPathNameW(openedPath.c_str(),
                static_cast<DWORD>(absolutePath.size()), absolutePath.data(), nullptr);
            if (!pathLength || pathLength >= absolutePath.size()) return E_FAIL;
            std::wstring widePath(absolutePath.data(), pathLength);
            identities_.Segment(absolutePath.data(), pathLength * sizeof(wchar_t));
            identities_.U32(read);
            identities_.Raw(digest.data(), digest.size());
            if (!identities_.Valid()) return E_FAIL;
            dependencies_.push_back({std::move(widePath), digest, read});
            const void* pointer = storage->data();
            buffers_.emplace(pointer, std::move(storage));
            total_ += read;
            *data = pointer;
            *bytes = read;
            return S_OK;
        } catch (...) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            return E_OUTOFMEMORY;
        }
    }
    virtual HRESULT WINAPI Close(LPCVOID data) noexcept {
        return buffers_.erase(data) == 1 ? S_OK : E_INVALIDARG;
    }
    std::vector<unsigned char> Identity() { return identities_.Take(); }
    bool Unchanged() const noexcept {
        for (const auto& dependency : dependencies_) {
            std::array<unsigned char, 32> current{};
            if (!Sha256File(dependency.path, current, kMaxPreprocessedBytes))
                return false;
            // SHA-256 of the empty input is computed by Sha256File, while the
            // preprocessor stores a zero digest for a zero-byte include.
            if (dependency.bytes != 0 && current != dependency.digest) return false;
            if (dependency.bytes == 0) {
                std::error_code error;
                if (std::filesystem::file_size(dependency.path, error) != 0 || error)
                    return false;
            }
        }
        return true;
    }
private:
    std::filesystem::path sourceDirectory_;
    struct Dependency {
        std::wstring path;
        std::array<unsigned char, 32> digest;
        DWORD bytes;
    };
    KeyBuilder identities_;
    std::vector<Dependency> dependencies_;
    std::unordered_map<const void*, std::unique_ptr<std::vector<unsigned char>>> buffers_;
    std::size_t total_ = 0;
    unsigned opens_ = 0;
};

// Creates one independent effect from immutable bytecode.  The object is only
// returned through created on success; on failure it is released here.  The
// caller owns created and the caller's outputs are not touched.
bool PackPayload(const std::vector<unsigned char>& bytecode,
                 const std::vector<unsigned char>& diagnostics,
                 HRESULT result, std::vector<unsigned char>& packed) {
    if (bytecode.empty() || diagnostics.size() > 256u * 1024u ||
        bytecode.size() + diagnostics.size() + 16 > kMaxPayloadBytes)
        return false;
    packed.clear();
    AppendU32(packed, 0x32505846u); // FXP2
    AppendU32(packed, static_cast<std::uint32_t>(bytecode.size()));
    AppendU32(packed, static_cast<std::uint32_t>(diagnostics.size()));
    AppendU32(packed, static_cast<std::uint32_t>(result));
    packed.insert(packed.end(), bytecode.begin(), bytecode.end());
    packed.insert(packed.end(), diagnostics.begin(), diagnostics.end());
    return true;
}

std::uint32_t PayloadU32(const unsigned char* bytes) noexcept {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

bool CreateEffectObject(const D3DX& hooks, IDirect3DDevice9* device,
                        const std::vector<unsigned char>& payload, DWORD flags,
                        void* pool, void*& created, void*& returnedDiagnostics,
                        HRESULT& createResult) noexcept {
    created = nullptr;
    returnedDiagnostics = nullptr;
    createResult = E_FAIL;
    if (!hooks.createEffect || payload.size() < 17 ||
        payload.size() > kMaxPayloadBytes ||
        PayloadU32(payload.data()) != 0x32505846u)
        return false;
    const auto codeBytes = PayloadU32(payload.data() + 4);
    const auto errorBytes = PayloadU32(payload.data() + 8);
    const HRESULT savedResult = static_cast<HRESULT>(PayloadU32(payload.data() + 12));
    if (!codeBytes || errorBytes > 256u * 1024u || FAILED(savedResult) ||
        std::uint64_t(codeBytes) + errorBytes + 16 != payload.size())
        return false;
    ScopedObject object(hooks);
    ScopedObject errors(hooks);
    ScopedObject savedErrors(hooks);
    if (errorBytes != 0) {
        if (!hooks.createBuffer ||
            FAILED(hooks.createBuffer(errorBytes, savedErrors.Address())) ||
            !savedErrors.Get())
            return false;
        void* destination = hooks.bufferPointer(savedErrors.Get());
        if (!destination || hooks.bufferSize(savedErrors.Get()) != errorBytes)
            return false;
        std::memcpy(destination, payload.data() + 16 + codeBytes, errorBytes);
    }
    const HRESULT hr = hooks.createEffect(
        device, payload.data() + 16, codeBytes, nullptr,
        nullptr, flags, pool, object.Address(), errors.Address());
    if (BufferHasText(hooks, errors.Get())) return false;
    if (FAILED(hr) || !object.Get()) return false;
    createResult = savedResult;
    created = object.Detach();
    returnedDiagnostics = savedErrors.Detach();
    return true;
}

// Hands a created object to the caller.  Assignment of pointers/HRESULT cannot
// fail, so a successful creation can always be committed.
void PublishCreatedEffect(void* created, void* diagnostics, HRESULT createResult,
                          void** effect, void** errors, HRESULT& result) noexcept {
    *effect = created;
    if (errors) *errors = diagnostics;
    result = createResult;
}

bool TryCreateEffectImpl(const D3DX& hooks, IDirect3DDevice9* device,
                         LPCWSTR filename, const void* macros,
                         const void* include, DWORD flags, void* pool,
                         void** effect, void** errors, HRESULT& result) {
    if (!hooks.identityValid || !hooks.preprocessFromFile ||
        !hooks.createCompiler || !hooks.createEffect || !hooks.compileEffect ||
        !hooks.bufferPointer || !hooks.bufferSize || !hooks.release) {
        LogBypass("d3dx-unavailable");
        return false;
    }
    if (CacheDisabled()) {
        LogBypass("cache-disabled");
        return false;
    }
    // A custom include handler belongs to the caller: the original entry point
    // must issue its include requests, so this path never preprocesses.
    if (include != nullptr) {
        LogBypass("custom-include");
        return false;
    }
    if (!device || !filename || !*filename || !effect) {
        LogBypass("invalid-parameters");
        return false;
    }
    std::vector<MacroEntry> macroEntries;
    if (!ReadMacros(macros, macroEntries)) {
        LogBypass("macros-unreadable");
        return false;
    }
    std::string path;
    if (!FullPathUtf8(filename, path)) {
        LogBypass("path-unreadable");
        return false;
    }
    // Allocate both snapshots before entering the noexcept validation callback.
    // Large automatic path buffers accumulate on MME's existing render stack.
    std::vector<wchar_t> directoryBefore(32768),directoryAfter(32768);
    const DWORD directoryLength = GetCurrentDirectoryW(static_cast<DWORD>(directoryBefore.size()), directoryBefore.data());
    if (!directoryLength || directoryLength >= directoryBefore.size()) return false;
    std::array<unsigned char, 32> sourceDigest{};
    if (!Sha256File(filename, sourceDigest, kMaxPreprocessedBytes)) return false;

    // Preprocess on every query using this DLL version's include lookup rule.
    // Record both expanded text and raw dependencies, including comments and
    // line breaks that can change diagnostic text without changing tokens.
    std::vector<unsigned char> text;
    EffectFileIncludes includes(filename, hooks.includesRelativeToSource);
    {
        ScopedObject textBuffer(hooks);
        ScopedObject preprocessErrors(hooks);
        const HRESULT preprocessResult = hooks.preprocessFromFile(
            filename, macros, &includes, textBuffer.Address(),
            preprocessErrors.Address());
        const bool preprocessDiagnostics =
            BufferHasText(hooks, preprocessErrors.Get());
        if (FAILED(preprocessResult) || !textBuffer.Get()) {
            LogBypass("preprocess-failed");
            return false;
        }
        if (preprocessDiagnostics) {
            LogBypass("preprocess-diagnostics");
            return false;
        }
        const void* textPointer = hooks.bufferPointer(textBuffer.Get());
        const DWORD textBytes = hooks.bufferSize(textBuffer.Get());
        if (!textPointer || textBytes == 0 ||
            textBytes > kMaxPreprocessedBytes) {
            LogBypass("preprocessed-too-large");
            return false;
        }
        ThrowIfFault(FaultPoint::kBeforePreprocessedCopy);
        text.assign(static_cast<const unsigned char*>(textPointer),
                    static_cast<const unsigned char*>(textPointer) + textBytes);
        // Scoped objects release the buffers on both the normal and the
        // throwing path (the copy above may allocate).
    }

    const auto snapshotUnchanged = [&]() noexcept {
        std::array<unsigned char, 32> sourceAfter{};
        const DWORD afterLength = GetCurrentDirectoryW(static_cast<DWORD>(directoryAfter.size()), directoryAfter.data());
        return afterLength == directoryLength &&
            wcscmp(directoryBefore.data(), directoryAfter.data()) == 0 &&
            Sha256File(filename, sourceAfter, kMaxPreprocessedBytes) &&
            sourceAfter == sourceDigest && includes.Unchanged();
    };
    if (!snapshotUnchanged()) {
        LogBypass("source-changed");
        return false;
    }
    KeyBuilder sourceIdentity;
    sourceIdentity.Segment(directoryBefore.data(), directoryLength * sizeof(wchar_t));
    sourceIdentity.Segment(filename, wcslen(filename) * sizeof(wchar_t));
    sourceIdentity.Raw(sourceDigest.data(), sourceDigest.size());
    const auto dependencies = includes.Identity();
    sourceIdentity.Segment(dependencies.data(), dependencies.size());
    if (!sourceIdentity.Valid()) return false;
    const auto sourceIdentityBytes = sourceIdentity.Take();
    std::vector<unsigned char> key;
    if (!BuildKey(hooks, path, flags, macroEntries, text, sourceIdentityBytes, key)) {
        LogBypass("key-too-large");
        return false;
    }
    std::array<unsigned char, 32> keyDigest{};
    if (!Sha256(key.data(), key.size(), keyDigest)) {
        LogBypass("key-digest-failed");
        return false;
    }
    const std::string shortKey = ShortKey(keyDigest);
    const std::wstring root = CacheDirectory();
    const std::wstring directory = EffectDirectory(root, filename);
    const std::wstring entryPath = directory.empty() ? std::wstring{} : EntryPath(directory, keyDigest);

    // 2. Memory first, then the disk entry.  Both stores only immutable
    //    bytecode; the effect object itself is always created per call below.
    std::vector<unsigned char> payload;
    if (MemoryGet(keyDigest, payload)) {
        if(!entryPath.empty())cache_manager::Touch(entryPath);
        void* created = nullptr;
        void* diagnostics = nullptr;
        HRESULT createResult = E_FAIL;
        if (!CreateEffectObject(hooks, device, payload, flags, pool, created, diagnostics,
                                createResult)) {
            LogFallback("cached-binary-rejected", shortKey.c_str());
            return false;
        }
        ScopedObject guard(hooks, created);
        ScopedObject diagnosticGuard(hooks, diagnostics);
        runtime_log::Write("MME_EFFECT_CACHE hit key=%s source=memory",
                           shortKey.c_str());
        PublishCreatedEffect(created, diagnostics, createResult, effect, errors, result);
        guard.Detach();
        if (errors) diagnosticGuard.Detach();
        return true;
    }
    if (!entryPath.empty() && ReadCacheEntry(entryPath, key, payload)) {
        cache_manager::Touch(entryPath);
        void* created = nullptr;
        void* diagnostics = nullptr;
        HRESULT createResult = E_FAIL;
        if (!CreateEffectObject(hooks, device, payload, flags, pool, created, diagnostics,
                                createResult)) {
            cache_manager::Invalidate(entryPath);
            LogFallback("cached-binary-rejected", shortKey.c_str());
            return false;
        }
        ScopedObject guard(hooks, created);
        ScopedObject diagnosticGuard(hooks, diagnostics);
        MemoryPut(keyDigest, payload);
        runtime_log::Write("MME_EFFECT_CACHE hit key=%s source=disk",
                           shortKey.c_str());
        PublishCreatedEffect(created, diagnostics, createResult, effect, errors, result);
        guard.Detach();
        if (errors) diagnosticGuard.Detach();
        return true;
    }
    runtime_log::Write("MME_EFFECT_CACHE miss key=%s", shortKey.c_str());

    // Compile the original file through this DLL's native file compiler, so
    // include lookup and diagnostic locations match its original entry point.
    // The memory-only compiler fallback is used by the test seam.
    bool preserveOriginalDiagnostics = false;
    {
        ScopedObject compiler(hooks);
        ScopedObject parseErrors(hooks);
        const HRESULT compilerResult = hooks.createCompilerFromFile
            ? hooks.createCompilerFromFile(filename, macros, nullptr, flags,
                  compiler.Address(), parseErrors.Address())
            : hooks.createCompiler(reinterpret_cast<const char*>(text.data()),
                  static_cast<UINT>(text.size()), nullptr, nullptr, flags,
                  compiler.Address(), parseErrors.Address());
        const bool parseDiagnostics =
            BufferHasText(hooks, parseErrors.Get());
        if (FAILED(compilerResult) || !compiler.Get()) {
            LogFallback("compiler-failed", shortKey.c_str());
            return false;
        }
        if (parseDiagnostics) {
            if (!hooks.originalFromFile || !hooks.createBuffer) {
                LogFallback("parse-diagnostics", shortKey.c_str());
                return false;
            }
            preserveOriginalDiagnostics = true;
        }
        ScopedObject binary(hooks);
        ScopedObject compileErrors(hooks);
        const HRESULT compileResult = hooks.compileEffect(
            compiler.Get(), flags, binary.Address(), compileErrors.Address());
        const bool compileDiagnostics =
            BufferHasText(hooks, compileErrors.Get());
        if (FAILED(compileResult) || !binary.Get()) {
            LogFallback("compile-failed", shortKey.c_str());
            return false;
        }
        if (compileDiagnostics) {
            if (!hooks.originalFromFile || !hooks.createBuffer) {
                LogFallback("compile-diagnostics", shortKey.c_str());
                return false;
            }
            preserveOriginalDiagnostics = true;
        }
        const void* binaryPointer = hooks.bufferPointer(binary.Get());
        const DWORD binaryBytes = hooks.bufferSize(binary.Get());
        if (!binaryPointer || binaryBytes == 0 ||
            binaryBytes > kMaxPayloadBytes) {
            LogFallback("bytecode-too-large", shortKey.c_str());
            return false;
        }
        ThrowIfFault(FaultPoint::kBeforeBytecodeCopy);
        payload.assign(static_cast<const unsigned char*>(binaryPointer),
                       static_cast<const unsigned char*>(binaryPointer) +
                           binaryBytes);
    }

    // 4. Create this call's own effect from the immutable binary.  Only after
    //    that succeeds is the bytecode stored.  Cache publication is best
    //    effort: a failure here must neither fail the load nor touch the
    //    caller's outputs a second time.
    void* created = nullptr;
    void* diagnostics = nullptr;
    HRESULT createResult = E_FAIL;
    std::vector<unsigned char> packed;
    if (preserveOriginalDiagnostics) {
        // On the first warning-producing compile, obtain the caller-visible
        // object and exact diagnostic bytes from its original entry point.
        // Subsequent hits recreate both independently without recompilation.
        ScopedObject originalObject(hooks);
        ScopedObject originalErrors(hooks);
        createResult = hooks.originalFromFile(device, filename, macros, nullptr,
            flags, pool, originalObject.Address(), originalErrors.Address());
        if (FAILED(createResult) || !originalObject.Get()) return false;
        bool packedOk = false;
        try {
            std::vector<unsigned char> diagnosticBytes;
            if (originalErrors.Get()) {
                const DWORD count = hooks.bufferSize(originalErrors.Get());
                const auto* pointer = static_cast<const unsigned char*>(
                    hooks.bufferPointer(originalErrors.Get()));
                if (!pointer || count == 0 || count > 256u * 1024u) {
                    PublishCreatedEffect(originalObject.Detach(),
                        errors ? originalErrors.Detach() : nullptr, createResult,
                        effect, errors, result);
                    return true;
                }
                diagnosticBytes.assign(pointer, pointer + count);
            }
            packedOk = PackPayload(payload, diagnosticBytes, createResult, packed);
        } catch (...) {
            packedOk = false;
        }
        if (!packedOk) {
            PublishCreatedEffect(originalObject.Detach(),
                errors ? originalErrors.Detach() : nullptr, createResult,
                effect, errors, result);
            return true;
        }
        created = originalObject.Detach();
        diagnostics = originalErrors.Detach();
    } else {
        if (!PackPayload(payload, {}, S_OK, packed) ||
            !CreateEffectObject(hooks, device, packed, flags, pool, created,
                                diagnostics, createResult)) {
            LogFallback("create-failed", shortKey.c_str());
            return false;
        }
    }
    ScopedObject guard(hooks, created);
    ScopedObject diagnosticGuard(hooks, diagnostics);
    // Source edits while compiling must never publish bytecode under an old
    // snapshot key. Return the successful effect but decline the cache write.
    const bool stable = snapshotUnchanged();
    try {
        ThrowIfFault(FaultPoint::kBeforeCachePublish);
        if (stable) {
            MemoryPut(keyDigest, packed);
            try {
                StoreOnDiskAndLog(root, directory, key, packed, keyDigest, shortKey);
            } catch (...) {
                runtime_log::Write(
                    "MME_EFFECT_CACHE store skipped reason=exception");
            }
        } else {
            runtime_log::Write("MME_EFFECT_CACHE store skipped reason=source-changed");
        }
    } catch (...) {
        runtime_log::Write("MME_EFFECT_CACHE store skipped reason=exception");
    }
    PublishCreatedEffect(created, diagnostics, createResult, effect, errors, result);
    guard.Detach();
    if (errors) diagnosticGuard.Detach();
    return true;
}

}  // namespace

bool ResolveD3DX(void* createEffectFromFile, D3DX& hooks) noexcept {
    try {
        return ResolveD3DXImpl(createEffectFromFile, hooks);
    } catch (...) {
        hooks = D3DX{};
        return false;
    }
}

bool TryCreateEffectWithHooks(const D3DX& hooks, IDirect3DDevice9* device,
                              LPCWSTR filename, const void* macros,
                              const void* include, DWORD flags, void* pool,
                              void** effect, void** errors,
                              HRESULT& result) noexcept {
    const DWORD incomingError = GetLastError();
    bool handled = false;
    try {
        handled = TryCreateEffectImpl(hooks, device, filename, macros, include,
                                      flags, pool, effect, errors, result);
    } catch (...) {
        runtime_log::Write("MME_EFFECT_CACHE fallback reason=cache-exception");
        handled = false;
    }
    SetLastError(incomingError);
    return handled;
}

bool TryCreateEffect(void* createEffectFromFile, IDirect3DDevice9* device,
                     LPCWSTR filename, const void* macros, const void* include,
                     DWORD flags, void* pool, void** effect, void** errors,
                     HRESULT& result) noexcept {
    const DWORD incomingError = GetLastError();
    bool handled = false;
    try {
        D3DX hooks;
        if (!ResolveD3DXImpl(createEffectFromFile, hooks)) {
            LogBypass("d3dx-unavailable");
        } else {
            handled = TryCreateEffectWithHooks(hooks, device, filename, macros,
                                               include, flags, pool, effect,
                                               errors, result);
        }
    } catch (...) {
        runtime_log::Write("MME_EFFECT_CACHE fallback reason=cache-exception");
        handled = false;
    }
    SetLastError(incomingError);
    return handled;
}

std::wstring CacheDirectory() noexcept {
    try {
        if (cache_storage_policy::MemoryOnly()) return {};
        const auto capacity=GetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR",nullptr,0);
        if(capacity && capacity<=32768) {
            std::vector<wchar_t> buffer(capacity);
            const auto length=GetEnvironmentVariableW(L"MMS_EFFECT_CACHE_DIR",buffer.data(),capacity);
            if(length && length<capacity)return std::wstring(buffer.data(),length);
        }
        const auto root=cache_manager::Root();
        return root.empty()?std::wstring{}:(root/L"MME").wstring();
    } catch (...) {
        return {};
    }
}

void ClearMemoryCache() noexcept {
    try {
        std::lock_guard<std::mutex> lock(memoryMutex);
        memoryEntries.clear();
        memoryOrder.clear();
        memoryBytes = 0;
    } catch (...) {
    }
}

void SetFaultPointForTesting(FaultPoint point) noexcept {
    g_faultPoint.store(static_cast<int>(point), std::memory_order_relaxed);
}

void SetDiskLimitsForTesting(std::size_t entries,
                             std::uint64_t bytes) noexcept {
    g_diskEntryLimit.store(entries ? entries : kMaxDiskEntries,
                           std::memory_order_relaxed);
    g_diskByteLimit.store(bytes ? bytes : kMaxDiskBytes,
                          std::memory_order_relaxed);
}

}  // namespace mikumikustudio::effect_compile_cache
