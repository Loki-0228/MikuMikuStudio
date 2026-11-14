// Linux diagnostic: link the production checkpoint codec unchanged.
// Reads only one bounded payload from stdin, never opens a cache directory.
#include "cache_catalog_codec.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace codec = mikumikustudio::cache_catalog::codec;
namespace files = mikumikustudio::cache_files;

template <std::size_t N>
std::string Hex(const std::array<std::uint8_t, N>& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(2 * N);
    for (auto item : value) {
        out.push_back(digits[item >> 4]);
        out.push_back(digits[item & 15]);
    }
    return out;
}

void Ref(const files::BlobInfo& ref) {
    // The decoder has already limited the name to ASCII hex plus .blob.
    const std::string name(ref.name.begin(), ref.name.end());
    std::cout << "{\"name\":\"" << name << "\",\"kind\":"
              << static_cast<std::uint32_t>(ref.spec.kind) << ",\"payload_sha256\":\""
              << Hex(ref.payloadDigest) << "\",\"payload_bytes\":" << ref.payloadBytes
              << ",\"declared_volume\":" << ref.identity.volume
              << ",\"declared_file_index\":" << ref.identity.fileIndex
              << ",\"declared_file_bytes\":" << ref.identity.fileBytes << '}';
}

int main() {
    std::array<char, codec::MaximumCheckpointBytes + 1> input{};
    std::cin.read(input.data(), static_cast<std::streamsize>(input.size()));
    if (std::cin.bad()) return 2;

    codec::CheckpointRecord checkpoint;
    const auto result = codec::DecodeCheckpoint(input.data(), static_cast<std::size_t>(std::cin.gcount()), checkpoint);
    if (result == codec::Result::Invalid) return 1;
    if (result != codec::Result::Ok) return 2;

    std::cout << "{\"revision\":" << checkpoint.state.revision
              << ",\"owner\":\"" << Hex(checkpoint.state.owner)
              << "\",\"retired_count\":" << checkpoint.retired.size() << ",\"snapshot\":";
    Ref(checkpoint.state.snapshot);
    std::cout << ",\"previous\":";
    Ref(checkpoint.state.previous);
    std::cout << "}\n";
    return std::cout ? 0 : 2;
}
