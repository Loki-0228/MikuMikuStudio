#include "mikumikustudio/native_mesh.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikumikustudio/effect_api.hpp"
#include <cstring>

namespace mikumikustudio {

const wchar_t* EffectMaterialNameWithFallback(EffectMaterialNameQuery query,
    std::uintptr_t object, int material, int english) {
    if (query == nullptr) return nullptr;
    const wchar_t* name = query(object, material, english);
    if (name != nullptr && *name != L'\0') return name;
    const wchar_t* alternate = query(object, material, english ? 0 : 1);
    return alternate != nullptr && *alternate != L'\0' ? alternate : name;
}

namespace {
EffectMaterialNameQuery originalMaterialName = nullptr;
HMODULE patchedEffect = nullptr;
HMODULE patchedHack = nullptr;
ULONG_PTR* patchedSlot = nullptr;

const wchar_t* __cdecl CompatibleMaterialName(std::uintptr_t object, int material,
                                             int english) {
    if (const auto* name=native_mesh::MaterialName(static_cast<uint32_t>(object),material,english!=0)) return name;
    return EffectMaterialNameWithFallback(originalMaterialName, object, material,
                                          english);
}

// Walk the loaded PE's declared imports using the platform's PE structures.
// No version-specific RVA, C++ object offset, code patch or private DLL data.
ULONG_PTR* FindMaterialNameImport(HMODULE module, ULONG_PTR original) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return nullptr;
    const auto size = nt->OptionalHeader.SizeOfImage;
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress >= size ||
        imports.Size > size - imports.VirtualAddress) return nullptr;
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + imports.VirtualAddress);
    for (std::size_t i = 0; i < imports.Size / sizeof(*descriptors); ++i) {
        const auto& descriptor = descriptors[i];
        if (!descriptor.Name) break;
        if (descriptor.Name >= size ||
            strnlen_s(reinterpret_cast<const char*>(base + descriptor.Name),
                      size - descriptor.Name) == size - descriptor.Name)
            continue;
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor.Name),
                     "MMHack.dll") != 0) continue;
        if (!descriptor.FirstThunk || descriptor.FirstThunk >= size) return nullptr;
        auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor.FirstThunk);
        const auto count = (size - descriptor.FirstThunk) / sizeof(*thunks);
        for (std::size_t j = 0; j < count && thunks[j].u1.Function; ++j)
            if (thunks[j].u1.Function == original) return &thunks[j].u1.Function;
    }
    return nullptr;
}
}

bool InstallEffectMaterialNameCompatibility() {
    const HMODULE effect = GetModuleHandleW(L"MMEffect.dll");
    const HMODULE hack = GetModuleHandleW(L"MMHack.dll");
    if (effect == nullptr || hack == nullptr) return false;
    const auto replacement = reinterpret_cast<ULONG_PTR>(&CompatibleMaterialName);
    if (effect == patchedEffect && hack == patchedHack && patchedSlot != nullptr &&
        *patchedSlot == replacement) return true;

    const auto query = reinterpret_cast<EffectMaterialNameQuery>(
        GetProcAddress(hack, "GetMaterialName"));
    if (query == nullptr) return false;
    auto* slot = FindMaterialNameImport(effect, reinterpret_cast<ULONG_PTR>(query));
    // An absent/different import is left alone, including another plugin's hook.
    if (slot == nullptr) return false;
    DWORD previousProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previousProtection))
        return false;
    originalMaterialName = query;
    const auto previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(replacement), reinterpret_cast<void*>(query));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), previousProtection, &ignored);
    if (previous != reinterpret_cast<void*>(query)) return false;
    patchedEffect = effect;
    patchedHack = hack;
    patchedSlot = slot;
    return true;
}

}  // namespace mikumikustudio
