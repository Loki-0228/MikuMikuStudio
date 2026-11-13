#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace mikumikustudio {
class MMDApp;
struct WaveAudioContext;
namespace cache_files { class RawSourceLease; }
namespace media {
bool IsAudioPath(const wchar_t* path);
bool IsVideoPath(const wchar_t* path);
bool IsModernVideoPath(const wchar_t* path);
// The source path survives project save/reload; only playback uses the temporary PCM file.
bool PrepareAudio(WaveAudioContext*, const wchar_t* source, std::wstring& playable, std::wstring& error,
    std::uint64_t maximumBytes = 0x7fffffffull - 44, bool* byteLimitExceeded = nullptr);
const wchar_t* AudioSource(WaveAudioContext*);
void ReleaseAudio(WaveAudioContext*);
// UI-thread ownership transfer accompanying a prepared context resource swap.
// Both keys remain valid; no decoder, file access or allocation is performed.
void SwapAudioCaches(WaveAudioContext*, WaveAudioContext*) noexcept;
bool OpenVideo(MMDApp*, const wchar_t* path, int& width, int& height, double& duration, std::wstring& error, int maxDimension = 0);
// Internal consuming entry point for a previously captured immutable source.
// The lease is consumed on success or failure. A successful decoder (including
// every FFmpeg restart) retains it independently of the caller. Failure leaves
// the current Video and width/height/duration unchanged. Capture, quota and
// durable ownership must be handled by the caller before this operation.
bool OpenVideoFromSource(MMDApp*, cache_files::RawSourceLease source,
    int& width, int& height, double& duration, std::wstring& error, int maxDimension = 0);
bool HasVideo(MMDApp*);
// UI-thread API. The bottom-up RGB32 DIB survives worker updates until the
// next UI read/close. Preview returns the last ready frame immediately;
// export and other callers requiring the exact frame use waitForFrame=true.
BITMAPINFOHEADER* VideoFrame(MMDApp*, double seconds, std::wstring& error, bool waitForFrame = true);
// Changes only when a different decoded frame is published, including reloads.
unsigned long long VideoFrameRevision(MMDApp*);
void ReleaseVideo(MMDApp*);
}
}
