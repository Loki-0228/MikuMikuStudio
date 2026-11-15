// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "model/pmd_geometry_wire.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using mikumikustudio::model_geometry_cache::VisitPmdGeometryWire;
using Bytes = std::vector<unsigned char>;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL PMD geometry wire: %s\n", message);
        std::exit(1);
    }
}

void PutLittleEndian32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<unsigned char>(value >> shift));
}
}

int main() {
    unsigned calls = 0;
    Check(VisitPmdGeometryWire(0, nullptr, 0, nullptr,
        [&](const unsigned char* data, unsigned size) {
            ++calls;
            const std::array<unsigned char, 8> zero{};
            return size == zero.size() && std::equal(zero.begin(), zero.end(), data);
        }) && calls == 1, "empty arrays produce both wire counts with no array access");

    constexpr std::uint32_t vertexCount = 401, indexCount = 2500;
    Bytes wire;
    PutLittleEndian32(wire, vertexCount);
    // Arbitrary source bytes, including negative zero and a signaling NaN.
    const std::array<unsigned char, 8> floatBits{0, 0, 0, 0x80, 0x45, 0x23, 0xa1, 0x7f};
    wire.insert(wire.end(), floatBits.begin(), floatBits.end());
    for (unsigned i = floatBits.size(); i < vertexCount * 38; ++i)
        wire.push_back(static_cast<unsigned char>((i * 13 + 7) ^ (i >> 5)));
    PutLittleEndian32(wire, indexCount);
    for (unsigned i = 0; i < indexCount * 2; ++i)
        wire.push_back(static_cast<unsigned char>((i * 31 + 11) ^ (i >> 7)));

    Bytes hostVertices(vertexCount * 40, 0xcd), hostIndices(indexCount * 4, 0xcb);
    for (unsigned i = 0; i < vertexCount; ++i)
        std::copy_n(wire.data() + 4 + i * 38, 38, hostVertices.data() + i * 40);
    for (unsigned i = 0; i < indexCount; ++i)
        std::copy_n(wire.data() + 8 + vertexCount * 38 + i * 2, 2, hostIndices.data() + i * 4);

    calls = 0;
    std::size_t position = 0;
    Check(VisitPmdGeometryWire(vertexCount, hostVertices.data(), indexCount, hostIndices.data(),
        [&](const unsigned char* data, unsigned size) {
            ++calls;
            if (!size || size > wire.size() - position) return false;
            if (!std::equal(data, data + size, wire.data() + position)) return false;
            position += size;
            return true;
        }) && position == wire.size() && calls > 1 && calls < 10,
        "source counts and raw bytes match across batches without host padding");
    calls = 0;
    Check(!VisitPmdGeometryWire(vertexCount, hostVertices.data(), indexCount, hostIndices.data(),
        [&](const unsigned char*, unsigned) { return ++calls < 2; }) && calls == 2,
        "a failed digest update stops all further input");
    std::puts("PASS PMD pristine wire bytes, empty arrays, and digest failure");
}
