#pragma once
#include "mikumikustudio/frame_limits.hpp"
#include <cstdint>

namespace mikumikustudio::plugins {
// Internal intent only; declaring these kinds does not enable command routing.
enum class SignedPasteCommand { Ordinary, MirrorBones };

// Preserve the clipboard's shared offset, including after family filtering.
// This arithmetic accepts signed offsets; source admission is a separate contract.
// Success writes one signed destination (zero is nonnegative); failure leaves it intact.
inline bool ResolveSignedPasteFrame(std::int64_t pasteOrigin,std::int64_t sourceOffset,
                                   std::int32_t& destination) noexcept {
    constexpr std::int64_t minimum=frame_limits::minimum;
    constexpr std::int64_t maximum=frame_limits::maximum;
    if(pasteOrigin<minimum||pasteOrigin>maximum)return false;
    // A validated origin makes both differences safe, even for hostile offsets.
    if(sourceOffset<minimum-pasteOrigin||sourceOffset>maximum-pasteOrigin)return false;
    destination=static_cast<std::int32_t>(pasteOrigin+sourceOffset);
    return true;
}
}
