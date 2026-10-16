#pragma once
#include "scene_geometry.hpp"
namespace mikudancestudio::plugins {
constexpr uint64_t bindDefinitionChannels = MMS_CHANNEL_RIGIDS | MMS_CHANNEL_JOINTS | MMS_CHANNEL_SHAPES | MMS_CHANNEL_MORPHS;
MmsResult CaptureBindDefinitions(SceneIdentity&, SceneData&, SceneData::Channels&,
    const void* token, uint32_t kind, uint64_t channels, GeometryDigests&);
}
