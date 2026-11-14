#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "mikumikustudio/ui_language.hpp"
namespace mikumikustudio {
struct DShowRecorder;
namespace recording_output_path { struct Selection; }
constexpr std::int32_t kNativePngMovCodec = -4;
// Pin the user's approved destination identity (or inspect it for direct
// callers) before opening a private staging file in the output directory.
bool StartPngMovRecording(DShowRecorder*, const wchar_t*, int, int, float, std::wstring&,
                          const recording_output_path::Selection* approved = nullptr,
                          bool* destinationChanged = nullptr);
bool IsPngMovRecording(const DShowRecorder*);
bool WritePngMovFrame(DShowRecorder*, const void*, std::size_t);
bool FinishPngMovRecording(DShowRecorder*, std::uint64_t, std::wstring&,
                           unsigned char uiLanguage = kUiEnglish);
void AbortPngMovRecording(DShowRecorder*);
}
