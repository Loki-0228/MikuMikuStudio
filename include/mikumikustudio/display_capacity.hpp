#pragma once

#include "mikumikustudio/model.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace mikumikustudio::mdl {
// MME reads the legacy layouts directly. Keep the byte/word mirrors saturated
// and attach the complete counts and indices to their owning allocations.
struct DisplayCapacity {
    std::uint32_t groups{}, faces{};
    std::array<std::uint32_t, 200> rowTypes{};
};
inline std::mutex displayCapacityMutex;
inline std::unordered_map<const void*, DisplayCapacity> displayCapacities;
inline std::unordered_map<const FrameGroup*, std::array<std::uint32_t, 2>> displayIndices;

inline void SetDisplayCapacity(ModelRecord& model, std::uint32_t groups, std::uint32_t faces) {
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    displayCapacities.insert_or_assign(model.groupNames, DisplayCapacity{groups, faces});
    model.groupCount = static_cast<unsigned char>((std::min)(groups, 255u));
    model.facialFrameCount = static_cast<unsigned char>((std::min)(faces, 255u));
}
inline std::uint32_t DisplayGroupCount(const ModelRecord& model) {
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    const auto found = displayCapacities.find(model.groupNames);
    return found == displayCapacities.end() ? model.groupCount : found->second.groups;
}
inline std::uint32_t FacialFrameCount(const ModelRecord& model) {
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    const auto found = displayCapacities.find(model.groupNames);
    return found == displayCapacities.end() ? model.facialFrameCount : found->second.faces;
}
inline void SetFrameIndices(FrameGroup& row, std::uint32_t group, std::uint32_t target) {
    if (group >= 65535 || target >= 65535) {
        std::lock_guard<std::mutex> lock(displayCapacityMutex);
        displayIndices.insert_or_assign(&row, std::array<std::uint32_t, 2>{group, target});
    }
    row.groupIndex = static_cast<std::uint16_t>((std::min)(group, 65535u));
    row.targetIndex = static_cast<std::uint16_t>((std::min)(target, 65535u));
}
inline std::uint32_t FrameGroupIndex(const FrameGroup& row) {
    if (row.groupIndex < 65535) return row.groupIndex;
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    const auto found = displayIndices.find(&row);
    return found == displayIndices.end() ? row.groupIndex : found->second[0];
}
inline std::uint32_t FrameTargetIndex(const FrameGroup& row) {
    if (row.targetIndex < 65535) return row.targetIndex;
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    const auto found = displayIndices.find(&row);
    return found == displayIndices.end() ? row.targetIndex : found->second[1];
}
inline void SetDisplayRowType(ModelRecord& model, std::size_t row, std::uint32_t type) {
    if (row >= 200) return;
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    const auto found = displayCapacities.find(model.groupNames);
    if (found != displayCapacities.end()) found->second.rowTypes[row] = type;
    model.boneListRowType[row] = static_cast<unsigned char>((std::min)(type, 255u));
}
inline std::uint32_t DisplayRowType(const ModelRecord& model, std::size_t row) {
    if (row >= 200) return 0;
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    const auto found = displayCapacities.find(model.groupNames);
    return found == displayCapacities.end() ? model.boneListRowType[row] : found->second.rowTypes[row];
}
inline void ForgetDisplayCapacity(const ModelRecord& model) {
    std::lock_guard<std::mutex> lock(displayCapacityMutex);
    const auto found = displayCapacities.find(model.groupNames);
    const auto faces = found == displayCapacities.end() ? model.facialFrameCount : found->second.faces;
    for (std::uint32_t i = 0; i < faces && model.displayFrames; ++i) displayIndices.erase(model.displayFrames + i);
    const auto* rows = static_cast<const FrameGroup*>(model.rbGroups);
    for (std::uint32_t i = 0; i < model.rigidBodyCount && rows; ++i) displayIndices.erase(rows + i);
    displayCapacities.erase(model.groupNames);
}
} // namespace mikumikustudio::mdl
