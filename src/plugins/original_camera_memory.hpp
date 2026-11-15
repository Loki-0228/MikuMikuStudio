#pragma once
#include "common.hpp"
#include <mms/legacy_camera.h>

namespace mikumikustudio::plugins {
inline constexpr char kOriginalFirstPersonCameraSha256[] =
    "d3515d48da411d15a55e09eefd9372fc4e9624405a4f1cf863d8dad5ce64a1ef";

// FirstPersonCamera v0.01 AMD64, audited against its original image. This is
// a supported field view, not the complete MMDMainData definition. In particular
// its MMD 9.26 distance offset differs from the later reference header.
struct OriginalCameraFields926 {
    unsigned char reserved0[0x328];
    unsigned char cameraMode;
    unsigned char reserved1[0x348 - 0x329];
    float rotation[3];
    unsigned char reserved2[0x36c - 0x354];
    float position[3];
    unsigned char reserved3[0xa18f8 - 0x378];
    float distance;
};
static_assert(offsetof(OriginalCameraFields926, cameraMode) == 0x328);
static_assert(offsetof(OriginalCameraFields926, rotation) == 0x348);
static_assert(offsetof(OriginalCameraFields926, position) == 0x36c);
static_assert(offsetof(OriginalCameraFields926, distance) == 0xa18f8);
static_assert(sizeof(OriginalCameraFields926) == 0xa18fc && alignof(OriginalCameraFields926) == 4);
static_assert(sizeof(void*) == 8);

class OriginalCameraMemory {
    void* reservation_ = nullptr;
    void* anchor_ = nullptr;
    HMODULE module_ = nullptr;
    void** import_ = nullptr;
    void* previousImport_ = nullptr;
    DWORD thread_ = 0;
    bool mode_ = false;
    static HMODULE WINAPI ModuleHandle(LPCWSTR);
public:
    OriginalCameraMemory();
    ~OriginalCameraMemory();
    OriginalCameraMemory(const OriginalCameraMemory&) = delete;
    OriginalCameraMemory& operator=(const OriginalCameraMemory&) = delete;
    // Called on the UI thread after verified cache loading and before create4.
    void Bind(HMODULE);
    void Sync(const MmsLegacyCameraStateV1&, bool cameraMode) noexcept;
    bool ReadChanges(MmsLegacyCameraStateV1&) const noexcept;
    bool CameraMode() const noexcept { return mode_; }
    const void* Data() const noexcept { return reservation_; }
};
}
