#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace repair {
using Bytes = std::vector<std::uint8_t>;
struct CameraKey {
    std::uint32_t index = 0, frame = 0, previous = 0, next = 0;
    std::size_t offset = 0;
    std::array<std::uint32_t, 8> raw{}; // xyz, rotation xyz, distance, fov
    std::array<std::array<double, 4>, 6> controls{};
    std::int32_t parentModel = -1, parentBone = 0;
    std::uint8_t perspective = 0;
};
enum class Evidence { AuthoredCurve, OriginalVmd };
struct Patch {
    std::size_t offset;
    std::uint32_t before, after, frame;
    unsigned channel;
    Evidence evidence;
};
struct Analysis {
    Bytes original;
    std::vector<Patch> patches;
    std::size_t cameraKeys = 0, skippedChannels = 0;
    bool hasAuthoredCamera = false, usedVmd = false;
};
constexpr std::size_t kMaxFileBytes = 512 * 1024 * 1024;
std::vector<CameraKey> ReadCamera(const Bytes&, std::size_t standardSize);
std::vector<CameraKey> ReadVmd(const Bytes&);
float LegacyEndpoint(const std::array<double, 4>&);
double Value(std::uint32_t raw, unsigned channel);
std::uint32_t Stored(double value, unsigned channel);
Analysis Analyze(Bytes pmm, const Bytes& originalVmd = {});
Bytes Apply(const Analysis&);
Bytes ReadFile(const std::filesystem::path&);
// Creates files exclusively in the source directory. Source and sidecars remain intact.
std::filesystem::path SaveCopy(const Analysis&, const std::filesystem::path& source,
                              const std::wstring& report);
std::wstring Report(const Analysis&, unsigned language, bool detailed = false);
}
