#pragma once

#include "mikumikustudio/subrecord_layout.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mikumikustudio::pmx_prepare {

// PMX 2.0 permits at most four extra float4 UVs. SDEF with 32-bit bone
// indices is the largest accepted record: 97 prefix bytes plus 52 tail bytes.
constexpr std::size_t kMaxRecordBytes = 149;

template<class Reader>
inline std::int32_t ReadBoneIndex(Reader& reader, int size) {
    switch (size) {
    case 1: {
        std::int8_t value = 0;
        reader.Read(&value, 1);
        return value;
    }
    case 2: {
        std::uint16_t value = 0;
        reader.Read(&value, 2);
        return static_cast<std::int16_t>(value);
    }
    default: {
        std::int32_t value = 0;
        reader.Read(&value, 4);
        return value;
    }
    }
}

// The production path and diagnostic workers use identical field writes and
// arithmetic. Fields not supplied by the disk record remain untouched.
template<class Reader>
inline void DecodeVertex(Reader& reader, std::uint8_t additionalUv,
                         std::uint8_t boneIndexSize, mdl::PmxVertex& vertex) {
    reader.Read(&vertex, 32);
    for (int uv = 0; uv < additionalUv; ++uv) {
        for (int component = 0; component < 4; ++component) {
            float surplus = 0.0f;
            reader.Read(uv < 4
                            ? &vertex.additionalUvByComponent[component][uv]
                            : &surplus, 4);
        }
    }
    reader.Read(&vertex.weightType, 1);
    switch (vertex.weightType) {
    case mdl::PmxWeightType::bdef2:
        vertex.bone[0] = ReadBoneIndex(reader, boneIndexSize);
        vertex.bone[1] = ReadBoneIndex(reader, boneIndexSize);
        reader.Read(vertex.weight, 4);
        break;
    case mdl::PmxWeightType::bdef4:
        for (int bone = 0; bone < 4; ++bone)
            vertex.bone[bone] = ReadBoneIndex(reader, boneIndexSize);
        reader.Read(vertex.weight, 16);
        break;
    case mdl::PmxWeightType::sdef: {
        vertex.bone[0] = ReadBoneIndex(reader, boneIndexSize);
        vertex.bone[1] = ReadBoneIndex(reader, boneIndexSize);
        reader.Read(vertex.weight, 4);
        reader.Read(vertex.sdef.center, 12);
        reader.Read(vertex.sdef.r0Offset, 12);
        reader.Read(vertex.sdef.r1Offset, 12);
        const float weight = vertex.weight[0];
        float midpoint[3];
        for (int component = 0; component < 3; ++component)
            midpoint[component] =
                weight * vertex.sdef.r0Offset[component]
                + (1.0f - weight) * vertex.sdef.r1Offset[component];
        for (int component = 0; component < 3; ++component) {
            vertex.sdef.r0Offset[component] -= midpoint[component];
            vertex.sdef.r1Offset[component] -= midpoint[component];
        }
        break;
    }
    default:
        vertex.bone[0] = ReadBoneIndex(reader, boneIndexSize);
        break;
    }
    reader.Read(&vertex.edgeScale, 4);
    vertex.hasPmxEdgeData = 1;
}

class MemoryReader {
public:
    explicit MemoryReader(const unsigned char* bytes) : current_(bytes) {}
    int Read(void* output, unsigned count) noexcept {
        std::memcpy(output, current_, count);
        current_ += count;
        return static_cast<int>(count);
    }
private:
    const unsigned char* current_;
};

inline unsigned TailBytes(std::uint8_t weightType,
                          std::uint8_t indexSize) noexcept {
    const unsigned indexBytes = indexSize == 1 || indexSize == 2 ? indexSize : 4;
    switch (static_cast<mdl::PmxWeightType>(weightType)) {
    case mdl::PmxWeightType::bdef2: return 2 * indexBytes + 4 + 4;
    case mdl::PmxWeightType::bdef4: return 4 * indexBytes + 16 + 4;
    case mdl::PmxWeightType::sdef: return 2 * indexBytes + 4 + 36 + 4;
    default: return indexBytes + 4;
    }
}

template<class Reader>
bool ReadPackedVertex(Reader& reader, std::uint8_t additionalUv,
                      std::uint8_t boneIndexSize, mdl::PmxVertex& output) {
    if (additionalUv > 4)
        return false;
    unsigned char bytes[kMaxRecordBytes];
    const unsigned prefixBytes = 32 + 16 * additionalUv + 1;
    if (reader.Read(bytes, prefixBytes) != static_cast<int>(prefixBytes))
        return false;
    const unsigned tail = TailBytes(bytes[prefixBytes - 1], boneIndexSize);
    if (reader.Read(bytes + prefixBytes, tail) != static_cast<int>(tail))
        return false;
    MemoryReader input(bytes);
    DecodeVertex(input, additionalUv, boneIndexSize, output);
    return true;
}

}  // namespace mikumikustudio::pmx_prepare
