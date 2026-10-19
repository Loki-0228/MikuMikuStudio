#pragma once
#include "mikumikustudio/d3d12_skinning.hpp"
namespace mikumikustudio {
class MMDApp;
struct GpuModelInput {
    std::vector<gpu::Vertex> vertices;
    std::vector<gpu::Bone> bones;
    std::vector<gpu::Material> materials;
    gpu::Parameters parameters{};
};
// Applies vertex/UV morphs then packs the GPU inputs; performs no CPU skinning.
bool PrepareGpuModelInput(MMDApp*, unsigned char*, const float world[16], GpuModelInput&, bool geometry = true);
}
