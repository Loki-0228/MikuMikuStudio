#pragma once

#include "negative_authoring.hpp"
#include "../plugins/native_signed_paste.hpp"

namespace mikumikustudio::negative_frames::authoring {
// Curves use absolute timeline coordinates. Translate their displayed keys
// and camera interval endpoints, but leave each camera slice's frozen source
// sampling domain intact: it is mapped from the translated interval at seek.
inline Ptr TranslatePaste(const Ptr& source, std::int32_t from, std::int32_t to) {
    if (!source || source->Semantics() != Mode::Authored || from == to) return source;
    const auto offset = std::int64_t(to) - from;
    const auto shift = [offset](std::int32_t& frame) {
        if (!plugins::ResolveSignedPasteFrame(frame, offset, frame))
            throw std::invalid_argument("negative paste curve frame out of range");
    };
    auto data = source->Author()->Data();
    for (auto& lane : data.lanes) {
        for (auto& key : lane.track.keys) shift(key.frame);
        for (auto& interval : lane.preservedCameraIntervals) {
            shift(interval.first);
            shift(interval.last);
        }
    }
    return Make(source->FamilyKind(), source->Semantics(), source->Native(), Freeze(data));
}
}
