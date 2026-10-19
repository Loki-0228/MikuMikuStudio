#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace mikumikustudio {
struct DShowRecorder;
constexpr std::int32_t kNativePngMovCodec = -4;
bool StartPngMovRecording(DShowRecorder*, const wchar_t*, int, int, float, std::wstring&);
bool IsPngMovRecording(const DShowRecorder*);
bool WritePngMovFrame(DShowRecorder*, const void*, std::size_t);
bool FinishPngMovRecording(DShowRecorder*, std::uint64_t, std::wstring&);
void AbortPngMovRecording(DShowRecorder*);
}
