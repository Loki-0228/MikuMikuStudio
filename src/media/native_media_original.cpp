#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/native_media.hpp"
#include "mikumikustudio/wave_audio_context.hpp"
#include "mikumikustudio/mf_h264_recorder.hpp"
#include "mikumikustudio/wic_mov_recorder.hpp"
#include <cwchar>

static_assert(MMD_ORIGINAL_PROFILE, "Original media provider must not enter the full build");

namespace mikumikustudio {
namespace {
constexpr const wchar_t* kUnavailable =
    L"This MMD 9.32 profile supports WAV audio and AVI video only.";
bool Extension(const wchar_t* path, const wchar_t* extension) {
    if (!path) return false;
    const wchar_t* dot = std::wcsrchr(path, L'.');
    return dot && _wcsicmp(dot, extension) == 0;
}
}

namespace media {
bool IsAudioPath(const wchar_t* path) { return Extension(path, L".wav"); }
bool IsVideoPath(const wchar_t* path) { return Extension(path, L".avi"); }
bool IsModernVideoPath(const wchar_t*) { return false; }
bool PrepareAudio(WaveAudioContext*, const wchar_t* source, std::wstring& playable, std::wstring& error) {
    playable.clear();
    error.clear();
    if (!IsAudioPath(source)) { error = kUnavailable; return false; }
    // The existing RIFF/WAV loader still validates and opens the actual file.
    playable = source;
    return true;
}
const wchar_t* AudioSource(WaveAudioContext* audio) { return audio ? audio->path : L""; }
void ReleaseAudio(WaveAudioContext*) {}
bool OpenVideo(MMDApp*, const wchar_t*, int& width, int& height, double& duration, std::wstring& error, int) {
    width = height = 0;
    duration = 0;
    error = kUnavailable;
    return false;
}
bool HasVideo(MMDApp*) { return false; }
BITMAPINFOHEADER* VideoFrame(MMDApp*, double, std::wstring& error, bool) { error = kUnavailable; return nullptr; }
unsigned long long VideoFrameRevision(MMDApp*) { return 0; }
void ReleaseVideo(MMDApp*) {}
}

// Recording lifecycle callers remain shared with the native AVI graph. No
// encoder, decoded file, COM object or capture session is owned here.
std::vector<std::wstring> EnumerateH264Encoders() { return {}; }
void ResetH264HardwareEncoderState() {}
bool ShowH264EncoderSettingsDialog(void*, bool, std::uint32_t&, int, int, float, unsigned char, void*) { return false; }
bool StartBuiltInH264Recording(DShowRecorder*, const wchar_t*, std::int32_t, std::int32_t,
    float, const WaveAudioContext*, bool, std::uint32_t, void*, unsigned char) { return false; }
bool IsBuiltInH264Recording(const DShowRecorder*) { return false; }
bool WriteBuiltInH264Frame(DShowRecorder*, const void*, std::size_t) { return false; }
bool FinishBuiltInH264Recording(DShowRecorder*, std::uint64_t) { return false; }
bool StartPngMovRecording(DShowRecorder*, const wchar_t*, int, int, float, std::wstring& error) {
    error = kUnavailable;
    return false;
}
bool IsPngMovRecording(const DShowRecorder*) { return false; }
bool WritePngMovFrame(DShowRecorder*, const void*, std::size_t) { return false; }
bool FinishPngMovRecording(DShowRecorder*, std::uint64_t, std::wstring& error) { error = kUnavailable; return false; }
void AbortPngMovRecording(DShowRecorder*) {}
}
