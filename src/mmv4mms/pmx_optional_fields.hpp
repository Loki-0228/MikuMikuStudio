#pragma once
#include "mikumikustudio/model.hpp"
#include "mmv/mmv.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace mikumikustudio::mmv4mms {
// PMX only initializes declared UV sets and SDEF data on SDEF vertices.
// The public ABI has fixed-size arrays, so absent fields must be zeroed.
inline void CopyPmxOptionalFields(const mdl::PmxVertex& source, uint32_t uvCount,
                                  MmvVertex& vertex, MmvWeight& weight) {
    if (uvCount > 4) throw std::runtime_error("Invalid additional UV count");
    std::fill(std::begin(vertex.additionalUv), std::end(vertex.additionalUv), 0.f);
    for (uint32_t set = 0; set < uvCount; ++set)
        for (int component = 0; component < 4; ++component)
            vertex.additionalUv[set * 4 + component] = source.additionalUvByComponent[component][set];
    std::fill(std::begin(weight.sdefCenter), std::end(weight.sdefCenter), 0.f);
    std::fill(std::begin(weight.sdefR0), std::end(weight.sdefR0), 0.f);
    std::fill(std::begin(weight.sdefR1), std::end(weight.sdefR1), 0.f);
    if (uint32_t(source.weightType) == 3) {
        std::memcpy(weight.sdefCenter, source.sdef.center, sizeof(weight.sdefCenter));
        std::memcpy(weight.sdefR0, source.sdef.r0Offset, sizeof(weight.sdefR0));
        std::memcpy(weight.sdefR1, source.sdef.r1Offset, sizeof(weight.sdefR1));
    }
}
}
