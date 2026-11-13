#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio {
struct DShowRecorder;
struct WaveAudioContext;
namespace recording_output_path { struct Selection; }

constexpr std::int32_t kBuiltInH264Codec = -2;
constexpr std::int32_t kSystemH264Codec = -3;

std::vector<std::wstring> EnumerateH264Encoders();
// Opens the full encoder-settings window for the Media Foundation H.264
// export.  width/height/fps are the pending export geometry: the window
// enumerates the options of the encoder the Sink Writer selects for those
// media types, so what is edited here is what the export applies.
bool ShowH264EncoderSettingsDialog(void* ownerWindow,
                                  bool enableSystemEncoders,
                                  std::uint32_t& averageBitrate,
                                  int width, int height, float fps,
                                  unsigned char uiLanguage, void* uiFont);

bool StartBuiltInH264Recording(DShowRecorder* recorder, const wchar_t* path,
                               std::int32_t width, std::int32_t height,
                               float fps, const WaveAudioContext* audio,
                               bool enableSystemEncoders,
                               std::uint32_t averageBitrate,
                               void* ownerWindow,
                               unsigned char uiLanguage,
                               const recording_output_path::Selection* approved = nullptr,
                               bool* destinationChanged = nullptr);
// An explicit re-selection of the system encoder allows a guarded hardware
// retry after a driver failure disabled it for this process.
void ResetH264HardwareEncoderState();
bool IsBuiltInH264Recording(const DShowRecorder* recorder);
// Packed bottom-up BGRA, matching the shared AVI/MOV readback buffer.
// The NV12 hardware path converts it to top-down planes before submission.
bool WriteBuiltInH264Frame(DShowRecorder* recorder, const void* bgra,
                           std::size_t bytes);
// Return the first encoding error as an HRESULT for the restored editor to show.
// An expected frame count of zero stops early. A finalized partial file is
// kept only when it will not replace an existing output.
// On a publication conflict, report where the previous video was kept if it
// could not be restored to the selected name.
bool FinishBuiltInH264Recording(DShowRecorder* recorder,
                                std::uint64_t expectedFrames,
                                std::int32_t* failure = nullptr,
                                std::wstring* retainedPrevious = nullptr);
}
