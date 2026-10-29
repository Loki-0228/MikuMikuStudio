#pragma once
#include <cstdint>
#include <vector>

namespace mikumikustudio::texture_cpu_decode {
using Bytes = std::vector<unsigned char>;
// Only true-colour 8-bit PNG and 24/32-bit TGA are accelerated. Other image
// types retain their original D3DX decoder, including authored DDS mipmaps.
bool Supported(const Bytes& input) noexcept;
// Produces one uncompressed A8R8G8B8 DDS level. D3DX on the caller still
// performs the original device-dependent resizing, filtering and mip creation.
bool Decode(const Bytes& input, unsigned width, unsigned height, Bytes& dds);
}
