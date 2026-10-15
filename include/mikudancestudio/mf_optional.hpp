#pragma once
#include <mfreadwrite.h>
#include <mftransform.h>

namespace mikudancestudio::media_compat {
// Windows SDK 10.0.19041 mfreadwrite.h. These declarations add no imports;
// availability is negotiated through QueryInterface, including on Win7.
inline constexpr GUID advancedVideoProcessing =
    {0x0f81da2c, 0xb537, 0x4672, {0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3}};
struct __declspec(uuid("588d72ab-5bc1-496a-8714-b70617141b25")) SinkWriterExtended : IMFSinkWriter {
    virtual HRESULT STDMETHODCALLTYPE GetTransformForStream(
        DWORD stream, DWORD index, GUID* category, IMFTransform** transform) = 0;
};
}
