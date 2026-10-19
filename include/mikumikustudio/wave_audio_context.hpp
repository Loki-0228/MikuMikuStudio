#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace mikumikustudio {

struct WaveAudioContext {
    unsigned char* waveformMax;
    unsigned char* waveformMin;
    HDC timelineDC;
    HWND mainWindow;
    IDirectSound* directSound;
    IDirectSoundBuffer* streamingBuffer;
    WAVEFORMATEX format;
    FILE* parseStream;
    wchar_t path[256];
    std::int32_t bufferBytes;
    std::uintptr_t feedThread;
    std::int32_t dataSize;
    std::int32_t readCursor;
    std::int32_t dataOffset;
    HANDLE fileHandle;
    std::uint32_t stopFlag;
    std::int32_t failureCount;
    std::int32_t waveformColumns;
    std::uint8_t englishUI;
    std::uint8_t reserved[3];
    LONG volume;
};

// Keep the original context layout while synchronizing with the feed thread.
// Completion (2) must never be overwritten by a late stop request (1).
inline LONG WaveStopState(WaveAudioContext* audio) {
    return InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&audio->stopFlag), 0, 0);
}

inline void RequestWaveStop(WaveAudioContext* audio) {
    InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&audio->stopFlag), 1, 0);
}

inline void SetWaveStopState(WaveAudioContext* audio, LONG state) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&audio->stopFlag), state);
}

static_assert(offsetof(WaveAudioContext, stopFlag) % alignof(LONG) == 0,
              "wave stop flag must be aligned for interlocked access");

#if !defined(_WIN64)
static_assert(sizeof(WaveAudioContext) == 0x25C, "x86 WaveAudioContext ABI");
static_assert(offsetof(WaveAudioContext, format) == 0x18, "x86 wave format");
static_assert(offsetof(WaveAudioContext, path) == 0x30, "x86 wave path");
static_assert(offsetof(WaveAudioContext, volume) == 0x258, "x86 wave volume");
#endif

}  // namespace mikumikustudio
