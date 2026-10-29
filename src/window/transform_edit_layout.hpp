#pragma once
#include "mikumikustudio/build_profile.hpp"

namespace mikumikustudio::ui {
inline bool TransformEditShowsPrefix(int id) noexcept {
    // Accessory XYZ/RXYZ/Si, camera/bone XYZ/RXYZ/distance, bone scale XYZ.
    // Accessory Tr (485), morph weights and other bounded controls keep their
    // original alignment.
    return !build_profile::original && ((id>=478 && id<=484) ||
        (id>=544 && id<=550) || id==572 || id==575 || id==577);
}
}
