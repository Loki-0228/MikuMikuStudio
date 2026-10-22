#pragma once
#include "mikumikustudio/build_profile.hpp"

namespace mikumikustudio::ui {
// Accessory XYZ/RXYZ/Si, camera/bone XYZ/RXYZ/distance and bone scale XYZ
// clear ES_CENTER/ES_RIGHT so their narrow edit boxes start at the prefix.
// Non-targets: accessory transparency (485), morph weights and other bounded
// controls keep the original alignment.
inline bool TransformEditShowsPrefix(int id) noexcept {
    return !build_profile::original && ((id >= 478 && id <= 484) ||
        (id >= 544 && id <= 550) || id == 572 || id == 575 || id == 577);
}
}
