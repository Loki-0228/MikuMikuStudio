#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include "mikumikustudio/effect_apng.hpp"
#include "mikumikustudio/runtime_log.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mikumikustudio {
#if defined(_WIN64)
namespace {

constexpr DWORD kImageSize = 0xE7000;
constexpr DWORD kFileSize = 917504;
constexpr DWORD kVtableRva = 0xB2720;
constexpr DWORD kResetRva = 0x82C0;
constexpr char kSha256[] = "74b3f5882a3bad28131cbc57994e2f960938e6ca850fc7139018ce4822bd87dc";
constexpr unsigned char kResetCode[]{
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57,
    0x41, 0x54, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8D, 0xB9, 0x00, 0x01, 0x00,
    0x00, 0x48, 0x8B, 0xD9, 0x45, 0x33, 0xE4, 0x48, 0x8B, 0x0F, 0x48, 0x85,
    0xC9, 0x74, 0x09, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10, 0x4C, 0x89, 0x27,
    0x48, 0x8B, 0xB3, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x60,
    0x44, 0x89, 0x64, 0x24, 0x60, 0x48, 0x8B, 0x4E, 0x08, 0xFF, 0x15, 0xAD,
    0xC4, 0x09, 0x00, 0x85, 0xC0, 0x74, 0x03, 0x89, 0x46, 0x10, 0x48, 0x8B,
    0xB3, 0x98, 0x00, 0x00, 0x00, 0x8B, 0x6C, 0x24, 0x60, 0x48, 0x8D, 0x54,
    0x24, 0x68, 0x48, 0x8B, 0x4E, 0x08, 0x44, 0x89, 0x64, 0x24, 0x68, 0xFF,
    0x15, 0x8F, 0xC4, 0x09, 0x00, 0x85, 0xC0, 0x74, 0x03, 0x89, 0x46, 0x10,
    0x8B, 0x54, 0x24, 0x68, 0x48, 0x8B, 0x4B, 0x40, 0x48, 0x89, 0x7C, 0x24,
    0x38, 0x44, 0x89, 0x64, 0x24, 0x30, 0x41, 0xB9, 0x01, 0x00, 0x00, 0x00,
    0x44, 0x8B, 0xC5, 0xC7, 0x44, 0x24, 0x28, 0x15, 0x00, 0x00, 0x00, 0xC7,
    0x44, 0x24, 0x20, 0x00, 0x02, 0x00, 0x00, 0xFF, 0x15, 0xCF, 0xC3, 0x09,
    0x00, 0x48, 0x8B, 0x5C, 0x24, 0x70, 0x48, 0x8B, 0x6C, 0x24, 0x78, 0x48,
    0x83, 0xC4, 0x40, 0x41, 0x5C, 0x5F, 0x5E, 0xC3,
};
static_assert(sizeof(kResetCode) == 0xBC);

using ResetMethod = HRESULT(__fastcall*)(void*);
SRWLOCK installLock = SRWLOCK_INIT;
bool attempted = false; // Accessed only under installLock.
HMODULE retainedModule = nullptr;
ResetMethod originalReset = nullptr;
const void* expectedVtable = nullptr;
void* volatile* resetSlot = nullptr;
DWORD installationThread = 0;
volatile LONG correctionEnabled = 0;
// The context above is fully prepared before CAS publication and never changed
// afterwards, including failed rollback. A retained DLL reference owns its code.

struct Lock {
    Lock() { AcquireSRWLockExclusive(&installLock); }
    ~Lock() { ReleaseSRWLockExclusive(&installLock); }
};
struct ModuleReference {
    HMODULE value = nullptr;
    ~ModuleReference() { if (value) FreeLibrary(value); }
};
struct HashResources {
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~HashResources() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

bool VerifyFileHash(HMODULE module) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (!length || length >= sizeof(path) / sizeof(path[0])) {
        runtime_log::Write("MME_APNG install_skipped reason=module_path error=%lu", GetLastError());
        return false;
    }
    HashResources resources;
    // No write/delete sharing while hashing; never write the DLL on disk.
    resources.file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (resources.file == INVALID_HANDLE_VALUE) {
        runtime_log::Write("MME_APNG install_skipped reason=file_open error=%lu", GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(resources.file, &size) || size.QuadPart != kFileSize) {
        runtime_log::Write("MME_APNG install_skipped reason=file_size size=%lld", size.QuadPart);
        return false;
    }
    NTSTATUS status = BCryptOpenAlgorithmProvider(&resources.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
        status = BCryptCreateHash(resources.algorithm, &resources.hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) {
        runtime_log::Write("MME_APNG install_skipped reason=hash_create status=0x%08lx", static_cast<unsigned long>(status));
        return false;
    }
    unsigned char buffer[65536];
    DWORD remaining = kFileSize;
    while (remaining) {
        DWORD read = 0;
        const DWORD request = remaining < sizeof(buffer) ? remaining : static_cast<DWORD>(sizeof(buffer));
        if (!ReadFile(resources.file, buffer, request, &read, nullptr) || !read || read > remaining) {
            runtime_log::Write("MME_APNG install_skipped reason=file_read error=%lu", GetLastError());
            return false;
        }
        status = BCryptHashData(resources.hash, buffer, read, 0);
        if (status < 0) {
            runtime_log::Write("MME_APNG install_skipped reason=hash_data status=0x%08lx", static_cast<unsigned long>(status));
            return false;
        }
        remaining -= read;
    }
    unsigned char digest[32]{};
    status = BCryptFinishHash(resources.hash, digest, sizeof(digest), 0);
    if (status < 0) {
        runtime_log::Write("MME_APNG install_skipped reason=hash_finish status=0x%08lx", static_cast<unsigned long>(status));
        return false;
    }
    char text[65]{};
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(digest); ++i) {
        text[i * 2] = digits[digest[i] >> 4];
        text[i * 2 + 1] = digits[digest[i] & 15];
    }
    if (std::strcmp(text, kSha256) != 0) {
        runtime_log::Write("MME_APNG install_skipped reason=sha256 actual=%s", text);
        return false;
    }
    return true;
}

// SEH is confined to new validation/slot access, never the original MME method.
bool VerifyLoadedImage(HMODULE module) noexcept {
    __try {
        const auto* image = reinterpret_cast<const unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew >= 0x1000) return false;
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            pe->FileHeader.TimeDateStamp != 0x5331C51D || pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            pe->OptionalHeader.SizeOfImage != kImageSize || pe->OptionalHeader.CheckSum != 0xE19D9 ||
            std::memcmp(image + kResetRva, kResetCode, sizeof(kResetCode)) != 0) return false;
        constexpr DWORD slots[]{0x7770, 0x6780, 0x8230, 0x8290, 0x82C0};
        const auto* table = reinterpret_cast<const void* const*>(image + kVtableRva);
        for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i)
            if (table[i] != image + slots[i]) return false;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadSlot(void* volatile* slot, void*& value) noexcept {
    __try { value = *slot; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { value = nullptr; return false; }
}
bool ExchangeSlot(void* volatile* slot, void* replacement, void* expected, void*& previous) noexcept {
    __try { previous = InterlockedCompareExchangePointer(slot, replacement, expected); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { previous = nullptr; return false; }
}

enum class Invalidation { NotDynamic, Written, InvalidObject, InvalidHead, Changed, Fault };
Invalidation InvalidateFrame(void* object) noexcept {
    if (!object || reinterpret_cast<std::uintptr_t>(object) % 8) return Invalidation::InvalidObject;
    __try {
        auto* bytes = static_cast<unsigned char*>(object);
        if (*reinterpret_cast<const volatile std::uintptr_t*>(bytes) != reinterpret_cast<std::uintptr_t>(expectedVtable))
            return Invalidation::InvalidObject;
        const auto count = *reinterpret_cast<const volatile unsigned*>(bytes + 0x48);
        const auto texture = *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x100);
        const auto preloaded = *reinterpret_cast<const volatile unsigned char*>(bytes + 0x108);
        if (!count || !texture || preloaded) return Invalidation::NotDynamic;
        const auto head = *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x68);
        if (head < 0x10000 || head >= 0x800000000000ull || head % 8) return Invalidation::InvalidHead;
        const auto* sentinel = reinterpret_cast<const volatile unsigned char*>(head);
        if (sentinel[0x29] != 1) return Invalidation::InvalidHead;
        if (*reinterpret_cast<const volatile std::uintptr_t*>(bytes) != reinterpret_cast<std::uintptr_t>(expectedVtable) ||
            *reinterpret_cast<const volatile unsigned*>(bytes + 0x48) != count ||
            *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x100) != texture ||
            *reinterpret_cast<const volatile unsigned char*>(bytes + 0x108) != preloaded ||
            *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x68) != head || sentinel[0x29] != 1)
            return Invalidation::Changed;
        // Constructor 7231/7235 uses this same sentinel. nullptr is not valid:
        // SelectFrame(7FA0) treats every non-sentinel cache as an existing frame.
        *reinterpret_cast<volatile std::uintptr_t*>(bytes + 0x80) = head;
        return *reinterpret_cast<const volatile std::uintptr_t*>(bytes + 0x80) == head
            ? Invalidation::Written : Invalidation::Changed;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return Invalidation::Fault; }
}

HRESULT __fastcall CompatibleReset(void* object) {
    // Deliberately outside SEH: preserve the original call, exceptions and HRESULT.
    const HRESULT result = originalReset(object);
    const DWORD error = GetLastError();
    if (result == S_OK && GetCurrentThreadId() == installationThread &&
        InterlockedCompareExchange(&correctionEnabled, 0, 0) != 0) {
        const auto state = InvalidateFrame(object);
        if (state != Invalidation::NotDynamic)
            runtime_log::Write("MME_APNG reset_cache object=%p state=%u", object, static_cast<unsigned>(state));
    }
    SetLastError(error);
    return result;
}

bool InstalledLocked() noexcept {
    void* current = nullptr;
    return InterlockedCompareExchange(&correctionEnabled, 0, 0) != 0 && resetSlot &&
        ReadSlot(resetSlot, current) && current == reinterpret_cast<void*>(&CompatibleReset);
}
} // namespace

bool InstallEffectApngCompatibility() {
    Lock lock;
    if (attempted) return InstalledLocked();
    attempted = true;
    ModuleReference reference;
    const HMODULE candidate = GetModuleHandleW(L"MMEffect.dll");
    if (!candidate) {
        runtime_log::Write("MME_APNG install_skipped reason=native_mme_absent");
        return false;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(candidate), &reference.value)) {
        runtime_log::Write("MME_APNG install_skipped reason=module_reference error=%lu", GetLastError());
        return false;
    }
    if (!VerifyFileHash(reference.value)) return false;
    if (!VerifyLoadedImage(reference.value)) {
        runtime_log::Write("MME_APNG install_skipped reason=loaded_image_or_existing_hook");
        return false;
    }
    auto* image = reinterpret_cast<unsigned char*>(reference.value);
    auto* slot = reinterpret_cast<void* volatile*>(image + kVtableRva + 4 * sizeof(void*));
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(const_cast<void**>(slot), &memory, sizeof(memory)) ||
        memory.AllocationBase != reference.value || memory.State != MEM_COMMIT ||
        memory.Type != MEM_IMAGE || memory.Protect != PAGE_READONLY) {
        runtime_log::Write("MME_APNG install_skipped reason=vtable_page");
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(const_cast<void**>(slot), sizeof(void*), PAGE_READWRITE, &oldProtection)) {
        runtime_log::Write("MME_APNG install_skipped reason=make_writable error=%lu", GetLastError());
        return false;
    }
    // Prepare every field before making the replacement visible. From this point
    // retain the reference even on failure: another hook could capture our pointer
    // during the brief interval before a rollback removes it from the table.
    retainedModule = reference.value;
    reference.value = nullptr;
    originalReset = reinterpret_cast<ResetMethod>(image + kResetRva);
    expectedVtable = image + kVtableRva;
    installationThread = GetCurrentThreadId();
    resetSlot = slot;
    void* const original = reinterpret_cast<void*>(originalReset);
    void* const replacement = reinterpret_cast<void*>(&CompatibleReset);
    void* previous = nullptr;
    const bool exchanged = ExchangeSlot(slot, replacement, original, previous);
    const bool published = exchanged && previous == original;
    DWORD ignored = 0;
    if (!VirtualProtect(const_cast<void**>(slot), sizeof(void*), oldProtection, &ignored)) {
        const DWORD restoreError = GetLastError();
        void* rollbackPrevious = nullptr;
        // Only undo our own publication; never overwrite a later owner's hook.
        const bool rollbackAccess = !published || ExchangeSlot(slot, original, replacement, rollbackPrevious);
        const bool rolledBack = published && rollbackAccess && rollbackPrevious == replacement;
        const bool protectionRestored = VirtualProtect(const_cast<void**>(slot), sizeof(void*), oldProtection, &ignored) != FALSE;
        const DWORD retryError = protectionRestored ? ERROR_SUCCESS : GetLastError();
        void* current = nullptr;
        const bool readable = ReadSlot(slot, current);
        runtime_log::Write("MME_APNG install_failed reason=restore_protection error=%lu published=%d rollback_access=%d rolled_back=%d protection_restored=%d retry_error=%lu slot_readable=%d inactive_wrapper_remains=%d",
            restoreError, published ? 1 : 0, rollbackAccess ? 1 : 0, rolledBack ? 1 : 0,
            protectionRestored ? 1 : 0, retryError, readable ? 1 : 0,
            readable && current == replacement ? 1 : 0);
        // The wrapper, if still referenced, forwards only. No partial correction.
        return false;
    }
    if (!published) {
        runtime_log::Write("MME_APNG install_skipped reason=slot_changed_or_inaccessible exchanged=%d previous=%p",
                           exchanged ? 1 : 0, previous);
        return false;
    }
    InterlockedExchange(&correctionEnabled, 1);
    const bool installed = InstalledLocked();
    if (!installed) InterlockedExchange(&correctionEnabled, 0);
    runtime_log::Write("MME_APNG install_%s module=%p thread=%lu retained_until_exit=1",
                       installed ? "complete" : "superseded", static_cast<void*>(retainedModule), installationThread);
    return installed;
}

bool IsEffectApngCompatibilityInstalled() {
    Lock lock;
    return InstalledLocked();
}
#else
bool InstallEffectApngCompatibility() {
    runtime_log::Write("MME_APNG install_skipped reason=unsupported_architecture");
    return false;
}
bool IsEffectApngCompatibilityInstalled() { return false; }
#endif
} // namespace mikumikustudio
