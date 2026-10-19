#pragma once
#include <Windows.h>
#include <string>

namespace mikumikustudio {
class MMDApp;
struct WaveAudioContext;
namespace media {
bool IsAudioPath(const wchar_t* path);
bool IsVideoPath(const wchar_t* path);
bool IsModernVideoPath(const wchar_t* path);
// The source path survives project save/reload; only playback uses the temporary PCM file.
bool PrepareAudio(WaveAudioContext*, const wchar_t* source, std::wstring& playable, std::wstring& error);
const wchar_t* AudioSource(WaveAudioContext*);
void ReleaseAudio(WaveAudioContext*);
bool OpenVideo(MMDApp*, const wchar_t* path, int& width, int& height, double& duration, std::wstring& error, int maxDimension = 0);
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
