#pragma once
#include "scene_identity.hpp"
#include <array>
namespace mikudancestudio::plugins {
using GeometryDigests = std::map<uint32_t, std::array<unsigned char, 32>>;
constexpr uint64_t bindGeometryChannels = MMS_CHANNEL_MESH | MMS_CHANNEL_BINDING |
    MMS_CHANNEL_MATERIALS | MMS_CHANNEL_TEXTURES;
MmsResult CaptureBindGeometry(SceneIdentity&, SceneData&, SceneData::Channels&,
    const void* token, uint32_t kind, uint64_t channels, GeometryDigests&);
}
