#include "original_camera_memory.hpp"
#include <intrin.h>
#include <cstring>

namespace mikumikustudio::plugins {
namespace {
// The audited DLL has one camera object. A thread-local binding also prevents
// unrelated callers or other modules from seeing its compatibility anchor.
thread_local OriginalCameraMemory* boundCamera = nullptr;
constexpr size_t kMainPointerRva = 0x1445f8;
constexpr size_t kGetterReturnRva = 0x109d;
constexpr size_t kModuleHandleIatRva = 0xf018;
void* Reserve(size_t bytes) {
    auto* value = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (!value) throw std::runtime_error("cannot reserve original camera memory view");
    return value;
}
void CommitPage(void* memory, size_t offset, size_t pageSize) {
    auto* page = static_cast<unsigned char*>(memory) + (offset / pageSize) * pageSize;
    if (!VirtualAlloc(page, pageSize, MEM_COMMIT, PAGE_READWRITE))
        throw std::runtime_error("cannot commit original camera memory fields");
}
}
OriginalCameraMemory::OriginalCameraMemory() : thread_(GetCurrentThreadId()) {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    try {
        reservation_ = Reserve(sizeof(OriginalCameraFields926));
        CommitPage(reservation_, 0, info.dwPageSize);
        CommitPage(reservation_, offsetof(OriginalCameraFields926, distance), info.dwPageSize);
        anchor_ = Reserve(kMainPointerRva + sizeof(void*));
        CommitPage(anchor_, kMainPointerRva, info.dwPageSize);
        auto** slot = reinterpret_cast<void**>(static_cast<unsigned char*>(anchor_) + kMainPointerRva);
        *slot = reservation_;
        DWORD previous = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READONLY, &previous))
            throw std::runtime_error("cannot protect original camera root pointer");
    } catch (...) {
        if (anchor_) VirtualFree(anchor_, 0, MEM_RELEASE);
        if (reservation_) VirtualFree(reservation_, 0, MEM_RELEASE);
        throw;
    }
}
OriginalCameraMemory::~OriginalCameraMemory() {
    if (import_) {
        DWORD previous = 0;
        if (VirtualProtect(import_, sizeof(void*), PAGE_READWRITE, &previous)) {
            InterlockedExchangePointer(import_, previousImport_);
            DWORD ignored = 0; VirtualProtect(import_, sizeof(void*), previous, &ignored);
        }
    }
    if (boundCamera == this) boundCamera = nullptr;
    if (anchor_) VirtualFree(anchor_, 0, MEM_RELEASE);
    if (reservation_) VirtualFree(reservation_, 0, MEM_RELEASE);
}
__declspec(noinline) HMODULE WINAPI OriginalCameraMemory::ModuleHandle(LPCWSTR name) {
    auto* camera = boundCamera;
    if (!name && camera && camera->thread_ == GetCurrentThreadId() &&
            _ReturnAddress() == reinterpret_cast<unsigned char*>(camera->module_) + kGetterReturnRva)
        return static_cast<HMODULE>(camera->anchor_);
    return GetModuleHandleW(name);
}
void OriginalCameraMemory::Bind(HMODULE module) {
    if (!module || module_ || boundCamera || thread_ != GetCurrentThreadId())
        throw std::runtime_error("original camera memory binding boundary");
    // Pin the observed call site as well as the loader's complete SHA-256 check.
    constexpr unsigned char getter[] = {0x33,0xc9,0xff,0x15,0x7b,0xdf,0x00,0x00,0x48,0x8d,0x98,0xf8,0x45,0x14,0x00};
    if (std::memcmp(reinterpret_cast<unsigned char*>(module) + 0x1095, getter, sizeof(getter)))
        throw std::runtime_error("original camera inline getter differs from verified layout");
    auto** slot = reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(module) + kModuleHandleIatRva);
    const auto expected = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetModuleHandleW");
    if (*slot != reinterpret_cast<void*>(expected))
        throw std::runtime_error("original camera module-handle import already changed");
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous))
        throw std::runtime_error("cannot bind original camera inline getter");
    module_ = module; import_ = slot; previousImport_ = *slot; boundCamera = this;
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&ModuleHandle));
    DWORD ignored = 0;
    if (!VirtualProtect(slot, sizeof(void*), previous, &ignored))
        throw std::runtime_error("cannot restore original camera import protection");
}
void OriginalCameraMemory::Sync(const MmsLegacyCameraStateV1& state, bool cameraMode) noexcept {
    auto& view = *static_cast<OriginalCameraFields926*>(reservation_);
    std::memcpy(view.position, state.position, sizeof(view.position));
    std::memcpy(view.rotation, state.rotation, sizeof(view.rotation));
    view.distance = state.distance; view.cameraMode = cameraMode ? 1 : 0; mode_ = cameraMode;
}
bool OriginalCameraMemory::ReadChanges(MmsLegacyCameraStateV1& state) const noexcept {
    const auto& view = *static_cast<const OriginalCameraFields926*>(reservation_);
    // Mode is a real, read-only mirror for this profile. Do not silently accept
    // unsupported writes, even though the pinned original only reads this byte.
    if (view.cameraMode != unsigned(mode_)) return false;
    std::memcpy(state.position, view.position, sizeof(view.position));
    std::memcpy(state.rotation, view.rotation, sizeof(view.rotation));
    state.distance = view.distance;
    return true;
}
}
