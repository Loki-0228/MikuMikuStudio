#pragma once
#include <cstring>

namespace mikumikustudio {
// PMD toonReference indexes the model's filename table, not the shared texture
// table. Any entry can name any toon01.bmp..toon10.bmp (MMD 9.32 x64 0xB79BC).
// Return the one-based shared texture slot, or zero for a custom filename.
inline int SharedToonSlot(const char* name) noexcept {
    if (!name || std::strncmp(name, "toon", 4) != 0) return 0;
    int slot = 0;
    if (name[4] == '0' && name[5] >= '1' && name[5] <= '9')
        slot = name[5] - '0';
    else if (name[4] == '1' && name[5] == '0')
        slot = 10;
    return slot && std::strcmp(name + 6, ".bmp") == 0 ? slot : 0;
}
}  // namespace mikumikustudio
